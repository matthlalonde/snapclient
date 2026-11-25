'use strict';

/**
 * Shared JavaScript utilities for ESP32 Snapclient UI
 */

// Get backend URL from query parameter (e.g., ?backend=http://192.168.1.100)
// This allows testing the UI locally while communicating with the actual device
function getBackendURL() {
  const urlParams = new URLSearchParams(window.location.search);
  const backend = urlParams.get('backend');
  if (backend) {
    console.log('Using backend:', backend);
    return backend;
  }
  // Default: use relative URLs (same server)
  return '';
}

const BACKEND_URL = getBackendURL();

// true unless ?backend= points at another origin
const BACKEND_SAME_ORIGIN = (() => {
  if (!BACKEND_URL) return true;
  try {
    return new URL(BACKEND_URL, location.href).origin === location.origin;
  } catch (e) {
    return false;
  }
})();

// a 401: the device has a settings password and this browser's login is
// missing or out of date
const LOGIN_NEEDED = 'Login needed: reload the page.';

// Headers for a script request to the device. With X-No-Login-Prompt it
// answers a missing login with a plain 401, so the browser shows no login
// prompt that a request timeout would then cancel. The pages say
// LOGIN_NEEDED instead, and the reload brings up the prompt.
function noLoginPrompt(headers = {}) {
  return Object.assign({ 'X-No-Login-Prompt': '1' }, headers);
}

// The same for a request to BACKEND_URL. Another origin (?backend=) gets no
// extra header: it would need a CORS preflight the device doesn't allow.
function backendHeaders(headers = {}) {
  return BACKEND_SAME_ORIGIN ? noLoginPrompt(headers) : Object.assign({}, headers);
}

// The device's reason when the last save or reset was refused (for example,
// the page was opened by a name it doesn't accept), for error messages
let lastSaveError = '';

// An Error with the device's answer text, or the status when there is none
async function responseError(response) {
  if (response.status === 401) return new Error(LOGIN_NEEDED);
  let text = '';
  try {
    text = (await response.text()).trim();
  } catch (e) {
    text = '';
  }
  return new Error(text ? text.slice(0, 200) : `HTTP error! status: ${response.status}`);
}

/**
 * Make a GET request to the device
 * @param {string} endpoint - API endpoint path
 * @returns {Promise<Response>} Fetch response
 */
async function getRequest(endpoint) {
  try {
    const response = await fetch(BACKEND_URL + endpoint, { headers: backendHeaders() });
    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }
    return response;
  } catch (error) {
    console.error(`Error fetching ${endpoint}:`, error);
    throw error;
  }
}

/**
 * Make a POST request to the device
 * @param {string} endpoint - API endpoint path
 * @param {object} data - Optional data to send
 * @returns {Promise<Response>} Fetch response
 */
async function postRequest(endpoint, data = null) {
  try {
    // the device only accepts changes with this header, which another
    // site's page can't send (so a ?backend= page on another origin can't
    // save either)
    const options = {
      method: 'POST',
      headers: backendHeaders({ 'X-Status-Action': '1' })
    };

    if (data) {
      options.headers['Content-Type'] = 'application/json';
      options.body = JSON.stringify(data);
    }
    
    const response = await fetch(BACKEND_URL + endpoint, options);
    if (!response.ok) {
      throw await responseError(response);
    }
    return response;
  } catch (error) {
    console.error(`Error posting to ${endpoint}:`, error);
    throw error;
  }
}

/**
 * Get a parameter value from the device
 * @param {string} paramKey - Parameter key
 * @returns {Promise<number|string|null>} Parameter value or null on error
 */
async function getParameter(paramKey) {
  try {
    const response = await getRequest(`/get?param=${paramKey}`);
    const value = await response.text();
    // Return as string for hostname and snapserver_host
    if (paramKey === 'hostname' || paramKey === 'snapserver_host') {
      return value.trim();
    }
    // For snapserver_port, handle empty string
    if (paramKey === 'snapserver_port') {
      const trimmed = value.trim();
      return trimmed === '' ? '' : parseFloat(trimmed);
    }
    return parseFloat(value);
  } catch (error) {
    console.error(`Error fetching parameter ${paramKey}:`, error);
    return null;
  }
}

/**
 * Update a parameter on the device
 * @param {string} paramKey - Parameter key
 * @param {number|string} value - Parameter value
 * @returns {Promise<boolean>} True if successful
 */
async function setParameter(paramKey, value) {
  lastSaveError = '';
  try {
    await postRequest(`/post?param=${paramKey}&value=${encodeURIComponent(value)}`);
    return true;
  } catch (error) {
    console.error(`Error setting parameter ${paramKey}:`, error);
    lastSaveError = error.message;
    return false;
  }
}

/**
 * Delete a parameter from NVS (clear to default)
 * @param {string} paramKey - Parameter key
 * @returns {Promise<boolean>} True if successful
 */
async function deleteParameter(paramKey) {
  lastSaveError = '';
  try {
    const response = await fetch(`/delete?param=${paramKey}`, {
      method: 'DELETE',
      headers: noLoginPrompt({ 'X-Status-Action': '1' }),  // see postRequest
    });
    if (!response.ok) {
      throw await responseError(response);
    }
    return true;
  } catch (error) {
    console.error(`Error deleting parameter ${paramKey}:`, error);
    lastSaveError = error.message;
    return false;
  }
}

/**
 * Show an error message to the user
 * @param {string} message - Error message to display
 * @param {HTMLElement} container - Container element to show error in
 */
function showError(message, container) {
  container.innerHTML = `<div class="error">${message}</div>`;
}

/**
 * Show a loading message to the user
 * @param {string} message - Loading message to display
 * @param {HTMLElement} container - Container element to show message in
 */
function showLoading(message, container) {
  container.innerHTML = `<div class="loading">${message}</div>`;
}