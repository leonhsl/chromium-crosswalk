// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/events/ozone/evdev/libgestures_glue/gesture_interpreter_libevdev_cros.h"

#include <gestures/gestures.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>

#include "base/strings/stringprintf.h"
#include "base/timer/timer.h"
#include "ui/events/event.h"
#include "ui/events/event_utils.h"
#include "ui/events/keycodes/dom4/keycode_converter.h"
#include "ui/events/ozone/evdev/cursor_delegate_evdev.h"
#include "ui/events/ozone/evdev/device_event_dispatcher_evdev.h"
#include "ui/events/ozone/evdev/event_device_info.h"
#include "ui/events/ozone/evdev/event_device_util.h"
#include "ui/events/ozone/evdev/keyboard_util_evdev.h"
#include "ui/events/ozone/evdev/libgestures_glue/gesture_property_provider.h"
#include "ui/events/ozone/evdev/libgestures_glue/gesture_timer_provider.h"
#include "ui/gfx/geometry/point_f.h"

namespace ui {

namespace {

// Convert libevdev device class to libgestures device class.
GestureInterpreterDeviceClass GestureDeviceClass(Evdev* evdev) {
  switch (evdev->info.evdev_class) {
    case EvdevClassMouse:
      return GESTURES_DEVCLASS_MOUSE;
    case EvdevClassMultitouchMouse:
      return GESTURES_DEVCLASS_MULTITOUCH_MOUSE;
    case EvdevClassTouchpad:
      return GESTURES_DEVCLASS_TOUCHPAD;
    case EvdevClassTouchscreen:
      return GESTURES_DEVCLASS_TOUCHSCREEN;
    default:
      return GESTURES_DEVCLASS_UNKNOWN;
  }
}

// Convert libevdev state to libgestures hardware properties.
HardwareProperties GestureHardwareProperties(
    Evdev* evdev,
    const GestureDeviceProperties* props) {
  HardwareProperties hwprops;
  hwprops.left = props->area_left;
  hwprops.top = props->area_top;
  hwprops.right = props->area_right;
  hwprops.bottom = props->area_bottom;
  hwprops.res_x = props->res_x;
  hwprops.res_y = props->res_y;
  hwprops.screen_x_dpi = 133;
  hwprops.screen_y_dpi = 133;
  hwprops.orientation_minimum = props->orientation_minimum;
  hwprops.orientation_maximum = props->orientation_maximum;
  hwprops.max_finger_cnt = Event_Get_Slot_Count(evdev);
  hwprops.max_touch_cnt = Event_Get_Touch_Count_Max(evdev);
  hwprops.supports_t5r2 = Event_Get_T5R2(evdev);
  hwprops.support_semi_mt = Event_Get_Semi_MT(evdev);
  /* buttonpad means a physical button under the touch surface */
  hwprops.is_button_pad = Event_Get_Button_Pad(evdev);
  return hwprops;
}

// Callback from libgestures when a gesture is ready.
void OnGestureReadyHelper(void* client_data, const Gesture* gesture) {
  GestureInterpreterLibevdevCros* interpreter =
      static_cast<GestureInterpreterLibevdevCros*>(client_data);
  interpreter->OnGestureReady(gesture);
}

// Convert gestures timestamp (stime_t) to ui::Event timestamp.
base::TimeDelta StimeToTimedelta(stime_t timestamp) {
  return base::TimeDelta::FromMicroseconds(timestamp *
                                           base::Time::kMicrosecondsPerSecond);
}

base::TimeDelta TimeValToTimeDelta(const timeval& tv) {
  return base::TimeDelta::FromMicroseconds(tv.tv_sec * 1000000 + tv.tv_usec);
}

// Number of fingers for scroll gestures.
const int kGestureScrollFingerCount = 2;

// Number of fingers for swipe gestures.
const int kGestureSwipeFingerCount = 3;

}  // namespace

GestureInterpreterLibevdevCros::GestureInterpreterLibevdevCros(
    int id,
    CursorDelegateEvdev* cursor,
    GesturePropertyProvider* property_provider,
    DeviceEventDispatcherEvdev* dispatcher)
    : id_(id),
      is_mouse_(false),
      cursor_(cursor),
      property_provider_(property_provider),
      dispatcher_(dispatcher),
      interpreter_(NULL),
      evdev_(NULL),
      device_properties_(new GestureDeviceProperties) {
  memset(&prev_key_state_, 0, sizeof(prev_key_state_));
}

GestureInterpreterLibevdevCros::~GestureInterpreterLibevdevCros() {
  // Note that this destructor got called after the evdev device node has been
  // closed. Therefore, all clean-up codes here shouldn't depend on the device
  // information (except for the pointer address itself).

  // Clean-up if the gesture interpreter has been successfully created.
  if (interpreter_) {
    // Unset callbacks.
    GestureInterpreterSetCallback(interpreter_, NULL, NULL);
    GestureInterpreterSetPropProvider(interpreter_, NULL, NULL);
    GestureInterpreterSetTimerProvider(interpreter_, NULL, NULL);
    DeleteGestureInterpreter(interpreter_);
    interpreter_ = NULL;
  }

  // Unregister device from the gesture property provider.
  GesturesPropFunctionsWrapper::UnregisterDevice(this);
}

void GestureInterpreterLibevdevCros::OnLibEvdevCrosOpen(
    Evdev* evdev,
    EventStateRec* evstate) {
  DCHECK(evdev->info.is_monotonic) << "libevdev must use monotonic timestamps";
  VLOG(9) << "HACK DO NOT REMOVE OR LINK WILL FAIL" << (void*)gestures_log;

  // Set device pointer and initialize properties.
  evdev_ = evdev;
  GesturesPropFunctionsWrapper::InitializeDeviceProperties(
      this, device_properties_.get());
  HardwareProperties hwprops =
      GestureHardwareProperties(evdev, device_properties_.get());
  GestureInterpreterDeviceClass devclass = GestureDeviceClass(evdev);
  is_mouse_ = property_provider_->IsDeviceIdOfType(id_, DT_MOUSE);

  // Create & initialize GestureInterpreter.
  DCHECK(!interpreter_);
  interpreter_ = NewGestureInterpreter();
  GestureInterpreterSetPropProvider(
      interpreter_,
      const_cast<GesturesPropProvider*>(&kGesturePropProvider),
      this);
  GestureInterpreterInitialize(interpreter_, devclass);
  GestureInterpreterSetHardwareProperties(interpreter_, &hwprops);
  GestureInterpreterSetTimerProvider(
      interpreter_,
      const_cast<GesturesTimerProvider*>(&kGestureTimerProvider),
      this);
  GestureInterpreterSetCallback(interpreter_, OnGestureReadyHelper, this);
}

void GestureInterpreterLibevdevCros::OnLibEvdevCrosEvent(Evdev* evdev,
                                                         EventStateRec* evstate,
                                                         const timeval& time) {
  // If the device has keys no it, dispatch any presses/release.
  DispatchChangedKeys(evdev, time);

  HardwareState hwstate;
  memset(&hwstate, 0, sizeof(hwstate));
  hwstate.timestamp = StimeFromTimeval(&time);

  // Mouse.
  hwstate.rel_x = evstate->rel_x;
  hwstate.rel_y = evstate->rel_y;
  hwstate.rel_wheel = evstate->rel_wheel;
  hwstate.rel_hwheel = evstate->rel_hwheel;

  // Touch.
  FingerState fingers[Event_Get_Slot_Count(evdev)];
  memset(&fingers, 0, sizeof(fingers));
  int current_finger = 0;
  for (int i = 0; i < evstate->slot_count; i++) {
    MtSlotPtr slot = &evstate->slots[i];
    if (slot->tracking_id == -1)
      continue;
    fingers[current_finger].touch_major = slot->touch_major;
    fingers[current_finger].touch_minor = slot->touch_minor;
    fingers[current_finger].width_major = slot->width_major;
    fingers[current_finger].width_minor = slot->width_minor;
    fingers[current_finger].pressure = slot->pressure;
    fingers[current_finger].orientation = slot->orientation;
    fingers[current_finger].position_x = slot->position_x;
    fingers[current_finger].position_y = slot->position_y;
    fingers[current_finger].tracking_id = slot->tracking_id;
    current_finger++;
  }
  hwstate.touch_cnt = Event_Get_Touch_Count(evdev);
  hwstate.finger_cnt = current_finger;
  hwstate.fingers = fingers;

  // Buttons.
  if (Event_Get_Button_Left(evdev))
    hwstate.buttons_down |= GESTURES_BUTTON_LEFT;
  if (Event_Get_Button_Middle(evdev))
    hwstate.buttons_down |= GESTURES_BUTTON_MIDDLE;
  if (Event_Get_Button_Right(evdev))
    hwstate.buttons_down |= GESTURES_BUTTON_RIGHT;

  GestureInterpreterPushHardwareState(interpreter_, &hwstate);
}

void GestureInterpreterLibevdevCros::SetAllowedKeys(
    scoped_ptr<std::set<DomCode>> allowed_keys) {
  if (!allowed_keys) {
    allowed_keys_.reset();
    return;
  }

  allowed_keys_.reset(new std::set<int>());
  for (const auto& it : *allowed_keys) {
    int evdev_code =
        NativeCodeToEvdevCode(KeycodeConverter::DomCodeToNativeKeycode(it));
    allowed_keys_->insert(evdev_code);
  }
}

void GestureInterpreterLibevdevCros::AllowAllKeys() {
  allowed_keys_.reset();
}

void GestureInterpreterLibevdevCros::OnGestureReady(const Gesture* gesture) {
  switch (gesture->type) {
    case kGestureTypeMove:
      OnGestureMove(gesture, &gesture->details.move);
      break;
    case kGestureTypeScroll:
      OnGestureScroll(gesture, &gesture->details.scroll);
      break;
    case kGestureTypeButtonsChange:
      OnGestureButtonsChange(gesture, &gesture->details.buttons);
      break;
    case kGestureTypeContactInitiated:
      OnGestureContactInitiated(gesture);
      break;
    case kGestureTypeFling:
      OnGestureFling(gesture, &gesture->details.fling);
      break;
    case kGestureTypeSwipe:
      OnGestureSwipe(gesture, &gesture->details.swipe);
      break;
    case kGestureTypeSwipeLift:
      OnGestureSwipeLift(gesture, &gesture->details.swipe_lift);
      break;
    case kGestureTypePinch:
      OnGesturePinch(gesture, &gesture->details.pinch);
      break;
    case kGestureTypeMetrics:
      OnGestureMetrics(gesture, &gesture->details.metrics);
      break;
    default:
      LOG(WARNING) << base::StringPrintf("Unrecognized gesture type (%u)",
                                         gesture->type);
      break;
  }
}

void GestureInterpreterLibevdevCros::OnGestureMove(const Gesture* gesture,
                                                   const GestureMove* move) {
  DVLOG(3) << base::StringPrintf("Gesture Move: (%f, %f) [%f, %f]",
                                 move->dx,
                                 move->dy,
                                 move->ordinal_dx,
                                 move->ordinal_dy);
  if (!cursor_)
    return;  // No cursor!

  cursor_->MoveCursor(gfx::Vector2dF(move->dx, move->dy));
  // TODO(spang): Use move->ordinal_dx, move->ordinal_dy
  dispatcher_->DispatchMouseMoveEvent(MouseMoveEventParams(
      id_, cursor_->GetLocation(), StimeToTimedelta(gesture->end_time)));
}

void GestureInterpreterLibevdevCros::OnGestureScroll(
    const Gesture* gesture,
    const GestureScroll* scroll) {
  DVLOG(3) << base::StringPrintf("Gesture Scroll: (%f, %f) [%f, %f]",
                                 scroll->dx,
                                 scroll->dy,
                                 scroll->ordinal_dx,
                                 scroll->ordinal_dy);
  if (!cursor_)
    return;  // No cursor!

  if (is_mouse_) {
    dispatcher_->DispatchMouseWheelEvent(MouseWheelEventParams(
        id_, cursor_->GetLocation(), gfx::Vector2d(scroll->dx, scroll->dy),
        StimeToTimedelta(gesture->end_time)));
  } else {
    dispatcher_->DispatchScrollEvent(ScrollEventParams(
        id_, ET_SCROLL, cursor_->GetLocation(),
        gfx::Vector2dF(scroll->dx, scroll->dy),
        gfx::Vector2dF(scroll->ordinal_dx, scroll->ordinal_dy),
        kGestureScrollFingerCount, StimeToTimedelta(gesture->end_time)));
  }
}

void GestureInterpreterLibevdevCros::OnGestureButtonsChange(
    const Gesture* gesture,
    const GestureButtonsChange* buttons) {
  DVLOG(3) << base::StringPrintf("Gesture Button Change: down=0x%02x up=0x%02x",
                                 buttons->down,
                                 buttons->up);

  if (!cursor_)
    return;  // No cursor!

  if (buttons->down & GESTURES_BUTTON_LEFT)
    DispatchMouseButton(BTN_LEFT, true, gesture->end_time);
  if (buttons->down & GESTURES_BUTTON_MIDDLE)
    DispatchMouseButton(BTN_MIDDLE, true, gesture->end_time);
  if (buttons->down & GESTURES_BUTTON_RIGHT)
    DispatchMouseButton(BTN_RIGHT, true, gesture->end_time);
  if (buttons->up & GESTURES_BUTTON_LEFT)
    DispatchMouseButton(BTN_LEFT, false, gesture->end_time);
  if (buttons->up & GESTURES_BUTTON_MIDDLE)
    DispatchMouseButton(BTN_MIDDLE, false, gesture->end_time);
  if (buttons->up & GESTURES_BUTTON_RIGHT)
    DispatchMouseButton(BTN_RIGHT, false, gesture->end_time);
}

void GestureInterpreterLibevdevCros::OnGestureContactInitiated(
    const Gesture* gesture) {
  // TODO(spang): handle contact initiated.
}

void GestureInterpreterLibevdevCros::OnGestureFling(const Gesture* gesture,
                                                    const GestureFling* fling) {
  DVLOG(3) << base::StringPrintf(
                  "Gesture Fling: (%f, %f) [%f, %f] fling_state=%d",
                  fling->vx,
                  fling->vy,
                  fling->ordinal_vx,
                  fling->ordinal_vy,
                  fling->fling_state);

  if (!cursor_)
    return;  // No cursor!

  EventType type =
      (fling->fling_state == GESTURES_FLING_START ? ET_SCROLL_FLING_START
                                                  : ET_SCROLL_FLING_CANCEL);

  // Fling is like 2-finger scrolling but with velocity instead of displacement.
  dispatcher_->DispatchScrollEvent(ScrollEventParams(
      id_, type, cursor_->GetLocation(), gfx::Vector2dF(fling->vx, fling->vy),
      gfx::Vector2dF(fling->ordinal_vx, fling->ordinal_vy),
      kGestureScrollFingerCount, StimeToTimedelta(gesture->end_time)));
}

void GestureInterpreterLibevdevCros::OnGestureSwipe(const Gesture* gesture,
                                                    const GestureSwipe* swipe) {
  DVLOG(3) << base::StringPrintf("Gesture Swipe: (%f, %f) [%f, %f]",
                                 swipe->dx,
                                 swipe->dy,
                                 swipe->ordinal_dx,
                                 swipe->ordinal_dy);

  if (!cursor_)
    return;  // No cursor!

  // Swipe is 3-finger scrolling.
  dispatcher_->DispatchScrollEvent(ScrollEventParams(
      id_, ET_SCROLL, cursor_->GetLocation(),
      gfx::Vector2dF(swipe->dx, swipe->dy),
      gfx::Vector2dF(swipe->ordinal_dx, swipe->ordinal_dy),
      kGestureSwipeFingerCount, StimeToTimedelta(gesture->end_time)));
}

void GestureInterpreterLibevdevCros::OnGestureSwipeLift(
    const Gesture* gesture,
    const GestureSwipeLift* swipelift) {
  DVLOG(3) << base::StringPrintf("Gesture Swipe Lift");

  if (!cursor_)
    return;  // No cursor!

  // Turn a swipe lift into a fling start.
  // TODO(spang): Figure out why and put it in this comment.

  dispatcher_->DispatchScrollEvent(ScrollEventParams(
      id_, ET_SCROLL_FLING_START, cursor_->GetLocation(),
      gfx::Vector2dF() /* delta */, gfx::Vector2dF() /* ordinal_delta */,
      kGestureScrollFingerCount, StimeToTimedelta(gesture->end_time)));
}

void GestureInterpreterLibevdevCros::OnGesturePinch(const Gesture* gesture,
                                                    const GesturePinch* pinch) {
  DVLOG(3) << base::StringPrintf(
                  "Gesture Pinch: dz=%f [%f]", pinch->dz, pinch->ordinal_dz);

  if (!cursor_)
    return;  // No cursor!

  NOTIMPLEMENTED();
}

void GestureInterpreterLibevdevCros::OnGestureMetrics(
    const Gesture* gesture,
    const GestureMetrics* metrics) {
  DVLOG(3) << base::StringPrintf("Gesture Metrics: [%f, %f] type=%d",
                                 metrics->data[0],
                                 metrics->data[1],
                                 metrics->type);
  NOTIMPLEMENTED();
}

void GestureInterpreterLibevdevCros::DispatchMouseButton(unsigned int button,
                                                         bool down,
                                                         stime_t time) {
  bool allow_remap = is_mouse_;
  dispatcher_->DispatchMouseButtonEvent(
      MouseButtonEventParams(id_, cursor_->GetLocation(), button, down,
                             allow_remap, StimeToTimedelta(time)));
}

void GestureInterpreterLibevdevCros::DispatchChangedKeys(Evdev* evdev,
                                                         const timeval& time) {
  unsigned long key_state_diff[EVDEV_BITS_TO_LONGS(KEY_CNT)];

  // Find changed keys.
  for (unsigned long i = 0; i < arraysize(key_state_diff); ++i)
    key_state_diff[i] = evdev->key_state_bitmask[i] ^ prev_key_state_[i];

  // Dispatch events for changed keys.
  for (unsigned long key = 0; key < KEY_CNT; ++key) {
    if (EvdevBitIsSet(key_state_diff, key)) {
      bool value = EvdevBitIsSet(evdev->key_state_bitmask, key);

      // Mouse buttons are handled by DispatchMouseButton.
      if (key >= BTN_MOUSE && key < BTN_JOYSTICK)
        continue;

      // Ignore digi buttons (e.g. BTN_TOOL_FINGER).
      if (key >= BTN_DIGI && key < BTN_WHEEL)
        continue;

      if (allowed_keys_ && !allowed_keys_->count(key))
        continue;

      // Dispatch key press or release to keyboard.
      dispatcher_->DispatchKeyEvent(
          KeyEventParams(id_, key, value, TimeValToTimeDelta(time)));
    }
  }

  // Update internal key state.
  for (unsigned long i = 0; i < EVDEV_BITS_TO_LONGS(KEY_CNT); ++i)
    prev_key_state_[i] = evdev->key_state_bitmask[i];
}

}  // namespace ui
