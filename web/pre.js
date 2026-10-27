// Saves and settings live in the browser's IndexedDB, mounted at /persist.
Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
    FS.mkdir('/persist');
    FS.mount(IDBFS, {}, '/persist');
    addRunDependency('persist');
    FS.syncfs(true, function (err) {
        if (err) console.warn('Could not load saved data', err);
        removeRunDependency('persist');
    });
});
