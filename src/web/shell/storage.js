/* storage.js - IndexedDB for anything too big for localStorage.
 *
 * Three stores, all keyed by the firmware's FNV-1a hash so two dumps never
 * collide - the same rule the desktop build uses for its save files:
 *
 *   firmware  the dumped images, so a reload does not ask for them again
 *   nvram     external flash plus the backup domain, i.e. the game's saves
 *   states    save-state slots, keyed "<id>:<slot>"
 *
 * None of it leaves the browser.
 */

const DB_NAME = 'gwemu';
const DB_VERSION = 1;

let dbp = null;

function open() {
    if (dbp) return dbp;
    dbp = new Promise((resolve, reject) => {
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = () => {
            const db = req.result;
            if (!db.objectStoreNames.contains('firmware')) db.createObjectStore('firmware');
            if (!db.objectStoreNames.contains('nvram')) db.createObjectStore('nvram');
            if (!db.objectStoreNames.contains('states')) db.createObjectStore('states');
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
    return dbp;
}

async function tx(store, mode, fn) {
    const db = await open();
    return new Promise((resolve, reject) => {
        const t = db.transaction(store, mode);
        const req = fn(t.objectStore(store));
        t.oncomplete = () => resolve(req ? req.result : undefined);
        t.onerror = () => reject(t.error);
        t.onabort = () => reject(t.error);
    });
}

export const storage = {
    get(store, key)         { return tx(store, 'readonly',  (s) => s.get(key)); },
    put(store, key, value)  { return tx(store, 'readwrite', (s) => s.put(value, key)); },
    del(store, key)         { return tx(store, 'readwrite', (s) => s.delete(key)); },
    keys(store)             { return tx(store, 'readonly',  (s) => s.getAllKeys()); },

    async available() {
        try { await open(); return true; } catch { return false; }
    },
};

/* Settings and key bindings are small and wanted synchronously at startup, so
   they live in localStorage instead. */
export const prefs = {
    load(defaults) {
        try {
            const raw = localStorage.getItem('gwemu.settings');
            if (!raw) return { ...defaults };
            return { ...defaults, ...JSON.parse(raw) };
        } catch {
            return { ...defaults };
        }
    },
    save(obj) {
        try { localStorage.setItem('gwemu.settings', JSON.stringify(obj)); } catch { /* private mode */ }
    },
};
