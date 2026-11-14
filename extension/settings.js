(() => {
  const policy = globalThis.NotionKexPolicy;
  const fields = Object.keys(policy.defaults);
  const status = document.getElementById('status');
  function render(value) {
    const settings = policy.normalize(value);
    for (const key of fields) {
      const field = document.getElementById(key);
      if (typeof settings[key] === 'boolean') field.checked = settings[key];
      else { field.value = settings[key]; document.getElementById(`${key}-value`).textContent = `${settings[key]}${key === 'caretDuration' ? ' ms' : ' px'}`; }
    }
  }
  function save() {
    const settings = {};
    for (const key of fields) {
      const field = document.getElementById(key);
      settings[key] = field.type === 'checkbox' ? field.checked : Number(field.value);
    }
    chrome.storage.local.set({ [policy.storageKey]: policy.normalize(settings) }, () => { status.textContent = chrome.runtime.lastError ? 'Could not save. Try again.' : 'Saved on this computer'; });
    render(settings);
  }
  chrome.storage.local.get(policy.storageKey, (result) => { render(result[policy.storageKey]); status.textContent = chrome.runtime.lastError ? 'Using defaults — storage unavailable' : 'Saved on this computer'; });
  fields.forEach((key) => document.getElementById(key).addEventListener('input', save));
  document.getElementById('reset').addEventListener('click', () => { render(policy.defaults); save(); });
})();
