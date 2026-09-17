        // ------------------------------------------------------------------
        // CentroidX: remember credentials, and pin the server key (TOFU).
        //
        // Everything below only overrides UI methods -- app/ui.js and core/
        // are stock, so a noVNC upgrade drops straight in.
        //
        // Credentials live in localStorage, never in defaults.json: that file
        // is served to anyone who can reach this page.
        // ------------------------------------------------------------------
        // Station-agnostic: unless defaults.json pins them, connect back to
        // whatever origin served this page.
        if (!defaults['host']) { defaults['host'] = location.hostname; }
        if (!defaults['port']) {
            defaults['port'] = location.port ||
                (location.protocol === 'https:' ? 443 : 80);
        }

        // Tab title. noVNC sets it from the VNC server's desktop name, which
        // is "Weston VNC backend" — accurate and useless to an operator. The
        // station name comes from NOVNC_STATION_NAME via the entrypoint, so
        // one image can serve "CentroidX - Frystar" and "CentroidX - Pokkun".
        let station = '';
        try {
            const r = await fetch('./branding.json');
            if (r.ok) { station = (await r.json()).station || ''; }
        } catch (err) {
            Log.Warn("Couldn't fetch branding.json: " + err);
        }
        const PAGE_TITLE = station ? `CentroidX - ${station}` : 'CentroidX';
        document.title = PAGE_TITLE;
        UI.updateDesktopName = (e) => {
            UI.desktopName = e.detail.name;
            document.title = PAGE_TITLE;
        };

        const REMEMBER = defaults['remember_credentials'] !== false;
        const KEY = n => `centroidx.${n}.${defaults['host'] || location.hostname}`;

        const store = (n, v) => { try { localStorage.setItem(KEY(n), v); } catch (e) { /* private mode */ } };
        const load  = n      => { try { return localStorage.getItem(KEY(n)); } catch (e) { return null; } };

        async function fingerprintOf(publickey) {
            // Same digest and formatting app/ui.js uses, so what we pin is
            // exactly what the dialog would have shown.
            const d = await window.crypto.subtle.digest("SHA-1", publickey);
            return Array.from(new Uint8Array(d).slice(0, 8))
                .map(x => x.toString(16).padStart(2, '0')).join('-');
        }

        // --- server key: approve silently when it matches the pinned one ---
        let pendingFingerprint = null;
        const origServerVerify  = UI.serverVerify.bind(UI);
        const origApproveServer = UI.approveServer.bind(UI);

        UI.serverVerify = async (e) => {
            if (e.detail.type !== 'RSA') { return origServerVerify(e); }
            pendingFingerprint = await fingerprintOf(e.detail.publickey);
            const pinned = REMEMBER ? load('serverkey') : null;
            if (pinned === pendingFingerprint) {
                Log.Info("Server key matches the pinned fingerprint; approving");
                UI.rfb.approveServer();
                return;
            }
            if (pinned !== null) {
                Log.Warn(`Server key CHANGED: pinned ${pinned}, offered ${pendingFingerprint}`);
            }
            return origServerVerify(e);   // unknown or changed -> ask a human
        };

        UI.approveServer = (e) => {
            if (REMEMBER && pendingFingerprint) { store('serverkey', pendingFingerprint); }
            return origApproveServer(e);
        };

        // --- credentials: send the remembered ones instead of prompting ---
        const origCredentials    = UI.credentials.bind(UI);
        const origSetCredentials = UI.setCredentials.bind(UI);

        UI.credentials = (e) => {
            if (!REMEMBER) { return origCredentials(e); }
            const types = e.detail.types;
            const have = {};
            if (types.includes('username')) { have.username = load('username'); }
            if (types.includes('password')) { have.password = load('password'); }
            const complete = types.every(t => t === 'username' || t === 'password')
                          && Object.values(have).every(v => v !== null && v !== '');
            if (!complete) { return origCredentials(e); }
            Log.Info("Using remembered credentials");
            UI.rfb.sendCredentials(have);
        };

        // Typed credentials are held here until the server accepts them.
        // Storing them on submit remembered a mistyped password, and
        // UI.credentials above then resent it on every connect without ever
        // showing the prompt again -- the only way out was the console.
        let typed = null;

        UI.setCredentials = (e) => {
            if (REMEMBER) {
                // Read before the original runs -- it clears the password field.
                typed = {
                    username: document.getElementById('noVNC_username_input').value,
                    password: document.getElementById('noVNC_password_input').value,
                };
            }
            return origSetCredentials(e);
        };

        const origConnectFinished = UI.connectFinished.bind(UI);
        const origSecurityFailed  = UI.securityFailed.bind(UI);

        UI.connectFinished = (e) => {
            if (REMEMBER && typed) {
                if (typed.username !== '') { store('username', typed.username); }
                if (typed.password !== '') { store('password', typed.password); }
            }
            typed = null;
            return origConnectFinished(e);
        };

        // Rejected: forget what was sent, whether typed or remembered, so the
        // next connect prompts. Covers a password changed on the station too.
        // The server key stays pinned -- the handshake got past it.
        UI.securityFailed = (e) => {
            typed = null;
            if (REMEMBER) {
                Log.Warn("Credentials rejected; forgetting the remembered ones");
                ['username', 'password'].forEach(n => {
                    try { localStorage.removeItem(KEY(n)); } catch (err) { /* private mode */ }
                });
            }
            return origSecurityFailed(e);
        };

        // Escape hatch: centroidxForget() from the console clears credentials
        // and the pinned server key.
        window.centroidxForget = () => {
            ['username', 'password', 'serverkey'].forEach(n => localStorage.removeItem(KEY(n)));
            return 'cleared';
        };

