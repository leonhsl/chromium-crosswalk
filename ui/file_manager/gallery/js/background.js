// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Configuration of the Gallery window.
 * @const
 * @type {Object}
 */
var windowCreateOptions = {
  id: 'gallery',
  innerBounds: {
    minWidth: 820,
    minHeight: 554
  },
  frame: 'none'
};

/**
 * Backgound object. This is necessary for AppWindowWrapper.
 * @type {!BackgroundBase}
 */
var background = new BackgroundBase();


// Initializes the strings. This needs for the volume manager.
var loadTimeDataPromise = new Promise(function(fulfill, reject) {
  chrome.fileManagerPrivate.getStrings(function(stringData) {
    loadTimeData.data = stringData;
    fulfill(true);
  });
});

// Initializes the volume manager. This needs for isolated entries.
var volumeManagerPromise = new Promise(function(fulfill, reject) {
  VolumeManager.getInstance(fulfill);
});

/**
 * Queue to serialize initialization.
 * @type {!Promise}
 */
window.initializePromise = Promise.all([loadTimeDataPromise,
                                     volumeManagerPromise]);

// Registers the handlers.
chrome.app.runtime.onLaunched.addListener(onLaunched);

/**
 * Called when an app is launched.
 *
 * @param {!Object} launchData Launch data. See the manual of chrome.app.runtime
 *     .onLaunched for detail.
 */
function onLaunched(launchData) {
  // Skip if files are not selected.
  if (!launchData || !launchData.items || launchData.items.length == 0)
    return;

  window.initializePromise.then(function() {
    var isolatedEntries = launchData.items.map(function(item) {
      return item.entry;
    });

    // Obtains entries in non-isolated file systems.
    // The entries in launchData are stored in the isolated file system.
    // We need to map the isolated entries to the normal entries to retrieve
    // their parent directory.
    chrome.fileManagerPrivate.resolveIsolatedEntries(
        isolatedEntries,
        function(externalEntries) {
          var urls = util.entriesToURLs(externalEntries);
          openGalleryWindow(urls, false);
        });
  });
}

/**
 * Returns a function to generate an ID for window.
 * @type {function():string} Function which returns an unique id.
 */
var generateWindowId = (function() {
  var seq = 0;
  return function() {
    return 'GALLERY_' + seq++;
  };
})();

/**
 * Opens gallery window.
 * @param {!Array.<string>} urls List of URL to show.
 * @param {boolean} reopen True if reopen, false otherwise.
 * @return {!Promise} Promise to be fulfilled on success, or rejected on error.
 */
function openGalleryWindow(urls, reopen) {
  return new Promise(function(fulfill, reject) {
    util.URLsToEntries(urls).then(function(result) {
      fulfill(util.entriesToURLs(result.entries));
    }).catch(reject);
  }).then(function(urls) {
    if (urls.length === 0)
      return Promise.reject('No file to open.');

    var windowId = generateWindowId();

    // Opens a window.
    return new Promise(function(fulfill, reject) {
      var gallery = new AppWindowWrapper('gallery.html',
          windowId,
          windowCreateOptions);

      gallery.launch(
          {urls: urls},
          reopen,
          fulfill.bind(null, gallery));
    }).then(function(gallery) {
      var galleryDocument = gallery.rawAppWindow.contentWindow.document;
      if (galleryDocument.readyState == 'complete')
        return gallery;

      return new Promise(function(fulfill, reject) {
        galleryDocument.addEventListener(
            'DOMContentLoaded', fulfill.bind(null, gallery));
      });
    });
  }).then(function(gallery) {
    gallery.rawAppWindow.focus();
    return gallery.rawAppWindow;
  }).catch(function(error) {
    console.error('Launch failed' + error.stack || error);
    return Promise.reject(error);
  });
}

// If is is run in the browser test, wait for the test resources are installed
// as a component extension, and then load the test resources.
if (chrome.test) {
  // Sets a global flag that we are in tests, so other components are aware of
  // it.
  window.IN_TEST = true;

  /** @type {string} */
  window.testExtensionId = 'ejhcmmdhhpdhhgmifplfmjobgegbibkn';
  chrome.runtime.onMessageExternal.addListener(function(message) {
    if (message.name !== 'testResourceLoaded')
      return;
    var script = document.createElement('script');
    script.src =
        'chrome-extension://' + window.testExtensionId +
        '/gallery/test_loader.js';
    document.documentElement.appendChild(script);
  });
}
