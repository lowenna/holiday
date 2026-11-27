    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Holiday Countdown</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #111; color: #eee; margin: 0; padding: 16px; }
    .card { background: #1c1c1e; max-width: 420px; margin: 0 auto 18px auto; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
    h2 { color: #4dd0e1; margin: 0 0 14px 0; font-size: 20px; }
    label.field { display: block; text-align: left; font-size: 13px; color: #aaa; margin: 12px 0 4px 0; }
    input[type=text], input[type=date], input[type=time] {
      width: 100%; box-sizing: border-box; padding: 10px; border-radius: 8px;
      border: 1px solid #333; background: #2c2c2e; color: #fff; font-size: 16px;
    }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 16px; text-align: left; }
    .chk { background: #2c2c2e; padding: 10px 12px; border-radius: 8px; display: flex; align-items: center; gap: 8px; font-size: 14px; cursor: pointer; }
    input[type=checkbox] { width: 18px; height: 18px; accent-color: #00acc1; }
    button { width: 100%; margin-top: 18px; background: #00acc1; color: #fff; border: none; padding: 12px; font-size: 16px; font-weight: bold; border-radius: 8px; cursor: pointer; }
    button:hover { background: #00838f; }
    .test-btn { background: #6a1b9a; margin-top: 10px; }
    .test-btn:hover { background: #4a148c; }
    .ota-btn { background: #37474f; }
    .ota-btn:hover { background: #455a64; }
    input[type=file] {
      display: block; width: 100%; box-sizing: border-box;
      padding: 12px; margin: 10px 0;
      background: #2c2c2e; border: 1px solid #444; border-radius: 8px;
      color: #eee; font-size: 16px;
    }
    input[type=file]::file-selector-button {
      background: #00acc1; color: #fff; border: none;
      padding: 8px 14px; margin-right: 12px;
      border-radius: 6px; font-size: 15px; font-weight: bold;
    }
    #progWrap { display: none; margin-top: 14px; background: #2c2c2e; border-radius: 8px; overflow: hidden; height: 22px; }
    #progBar { width: 0%; height: 100%; background: #00acc1; transition: width 0.2s; }
    #otaStatus, #bdayStatus, #ssaverStatus, #noteStatus { font-size: 13px; color: #aaa; margin-top: 8px; text-align: center; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Holiday Settings</h2>
    <form method="POST" action="/save">
      <label class="field">Destination</label>
      <input type="text" name="where" maxlength="20" value=")rawliteral" + String(gWhere) + R"rawliteral(" required>

      <label class="field">Departure Date</label>
      <input type="date" name="date" value=")rawliteral" + String(dateBuf.data()) + R"rawliteral(" required>

      <label class="field">Departure Time (UK)</label>
      <input type="time" name="time" value=")rawliteral" + String(timeBuf.data()) + R"rawliteral(" required>

      <label class="field">Birthday (Day &amp; Month)</label>
      <input type="date" name="bday" value=")rawliteral" + String(bdayBuf.data()) + R"rawliteral(">

      <div class="grid">
        <label class="chk"><input type="checkbox" name="ssaver" value="1" )rawliteral" + String(gScreensaverEnabled ? "checked" : "") + R"rawliteral(>Screensaver</label>
        <label class="chk"><input type="checkbox" name="dim" value="1" )rawliteral" + String(gDimmerEnabled ? "checked" : "") + R"rawliteral(>Auto-Dim</label>
        <label class="chk"><input type="checkbox" name="wxen" value="1" )rawliteral" + String(gWeatherEnabled ? "checked" : "") + R"rawliteral(>Weather</label>
        <label class="chk"><input type="checkbox" name="isl1h" value="1" )rawliteral" + String(gIslandPlusOne ? "checked" : "") + R"rawliteral(>Resort Time +1h</label>
        <label class="chk"><input type="checkbox" name="bdayen" value="1" )rawliteral" + String(gBirthdayEnabled ? "checked" : "") + R"rawliteral(>Birthday</label>
        <label class="chk"><input type="checkbox" name="rainpct" value="1" )rawliteral" + String(gRainAsPercent ? "checked" : "") + R"rawliteral(>Rain Chance (%)</label>
      </div>

      <button type="submit">Save &amp; Restart</button>
    </form>
    <button type="button" class="test-btn" id="testBdayBtn">Preview Birthday Screen</button>
    <div id="bdayStatus"></div>
    <button type="button" class="test-btn" id="testSsaverBtn" style="background:#00695c;">Preview Screensaver</button>
    <div id="ssaverStatus"></div>
  </div>

  <div class="card">
    <h2>Send Note to Display</h2>
    <p style="font-size:13px;color:#aaa;margin:0 0 10px 0">Pop up a message on the screen right now:</p>
    <input type="text" id="noteText" maxlength="90" placeholder="e.g. Don't forget passports!">
    <button type="button" id="sendNoteBtn" style="margin-top:12px;">Send to Screen</button>
    <div id="noteStatus"></div>
  </div>

  <div class="card">
    <h2>Software Update</h2>
    <p style="font-size:13px;color:#aaa;margin:0 0 6px 0">Device: <b>)rawliteral" + String(otaHostname) + R"rawliteral(.local</b></p>
    <p style="font-size:13px;color:#4dd0e1;margin:0 0 14px 0">Installed: <b>v)rawliteral" + String(FW_VERSION) + R"rawliteral(</b></p>

    <button type="button" id="checkGhBtn" style="margin-top:0;background:#00695c;">Check GitHub for Update</button>
    <div id="ghStatus" style="font-size:13px;color:#aaa;margin-top:8px;text-align:center;"></div>
    <button type="button" id="installGhBtn" style="display:none;background:#2e7d32;margin-top:10px;">Install Cloud Update</button>

    <hr style="border:0;border-top:1px solid #333;margin:18px 0;">

    <form id="otaForm">
      <label class="field" style="margin-top:0">Or upload manual .bin file:</label>
      <input type="file" id="fwFile" name="update" accept=".bin">
      <button type="submit" class="ota-btn" id="uploadBtn">Install Local File</button>
    </form>
    <div id="progWrap"><div id="progBar"></div></div>
    <div id="otaStatus"></div>
  </div>

  <script>
    const CURRENT_VER = ")rawliteral" + String(FW_VERSION) + R"rawliteral(";
    let cloudBinUrl = "";

    document.getElementById('sendNoteBtn').addEventListener('click', function() {
      const inp = document.getElementById('noteText');
      const nStatus = document.getElementById('noteStatus');
      const msg = inp.value.trim();
      if (!msg) {
        nStatus.style.color = '#ff8a80';
        nStatus.textContent = 'Type a short message first.';
        return;
      }
      nStatus.style.color = '#aaa';
      nStatus.textContent = 'Sending to display...';
      fetch('/sendnote?msg=' + encodeURIComponent(msg), { method: 'POST' })
        .then(function(r) { return r.text(); })
        .then(function(txt) {
          nStatus.style.color = '#69f0ae';
          nStatus.textContent = txt;
          inp.value = '';
        })
        .catch(function() {
          nStatus.style.color = '#ff8a80';
          nStatus.textContent = 'Could not send note.';
        });
    });

    document.getElementById('checkGhBtn').addEventListener('click', function() {
      const ghStatus = document.getElementById('ghStatus');
      const installBtn = document.getElementById('installGhBtn');
      ghStatus.style.color = '#aaa';
      ghStatus.textContent = 'Checking GitHub...';
      installBtn.style.display = 'none';

      fetch('/checkota')
        .then(function(r) {
          if (!r.ok) throw new Error('HTTP ' + r.status);
          return r.json();
        })
        .then(function(data) {
          if (data.version && data.version > CURRENT_VER) {
            cloudBinUrl = data.url;
            ghStatus.style.color = '#69f0ae';
            ghStatus.textContent = 'New version available: v' + data.version;
            installBtn.textContent = 'Install v' + data.version + ' Now';
            installBtn.style.display = 'block';
          } else {
            ghStatus.style.color = '#4dd0e1';
            ghStatus.textContent = 'You are on the latest version (v' + CURRENT_VER + ').';
          }
        })
        .catch(function(err) {
          ghStatus.style.color = '#ff8a80';
          ghStatus.textContent = 'Could not reach GitHub (' + err.message + ')';
        });
    });

    document.getElementById('installGhBtn').addEventListener('click', function() {
      if (!cloudBinUrl) return;
      const ghStatus = document.getElementById('ghStatus');
      this.disabled = true;
      ghStatus.style.color = '#ffd54f';
      ghStatus.textContent = 'Starting cloud update on device...';

      fetch('/cloudota?url=' + encodeURIComponent(cloudBinUrl), { method: 'POST' })
        .then(function() {
          ghStatus.textContent = 'Downloading & flashing on screen... Reloading in 15s.';
          setTimeout(function() { location.reload(); }, 15000);
        })
        .catch(function() {
          ghStatus.textContent = 'Update triggered! Reloading in 15s...';
          setTimeout(function() { location.reload(); }, 15000);
        });
    });

    document.getElementById('testBdayBtn').addEventListener('click', function() {
      const bStatus = document.getElementById('bdayStatus');
      bStatus.textContent = 'Showing birthday preview on screen...';
      fetch('/testbday', { method: 'POST' })
        .then(function(r) { return r.text(); })
        .then(function(txt) { bStatus.textContent = txt; })
        .catch(function() { bStatus.textContent = 'Triggered!'; });
    });

    document.getElementById('testSsaverBtn').addEventListener('click', function() {
      const sStatus = document.getElementById('ssaverStatus');
      sStatus.textContent = 'Starting hermit crab screensaver...';
      fetch('/testssaver', { method: 'POST' })
        .then(function(r) { return r.text(); })
        .then(function(txt) { sStatus.textContent = txt; })
        .catch(function() { sStatus.textContent = 'Triggered!'; });
    });

    const fwInput = document.getElementById('fwFile');
    const statusEl = document.getElementById('otaStatus');
    const progWrap = document.getElementById('progWrap');
    const progBar = document.getElementById('progBar');

    document.getElementById('otaForm').addEventListener('submit', function(e) {
      e.preventDefault();
      if (!fwInput.files || fwInput.files.length === 0) {
        statusEl.textContent = 'Please select an update file (.bin) first.';
        return;
      }
      const formData = new FormData();
      formData.append('update', fwInput.files.item(0));

      progWrap.style.display = 'block';
      progBar.style.width = '0%';
      statusEl.textContent = 'Uploading update...';
      document.getElementById('uploadBtn').disabled = true;

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);
      xhr.upload.onprogress = function(ev) {
        if (ev.lengthComputable) {
          const pct = Math.round((ev.loaded / ev.total) * 100);
          progBar.style.width = pct + '%';
          statusEl.textContent = 'Uploading: ' + pct + '%';
        }
      };
      xhr.onload = function() {
        if (xhr.status === 200) {
          progBar.style.width = '100%';
          statusEl.textContent = 'Update complete! Restarting device...';
          setTimeout(function() { location.reload(); }, 6000);
        } else {
          statusEl.textContent = 'Update failed (' + xhr.status + ')';
          document.getElementById('uploadBtn').disabled = false;
        }
      };
      xhr.onerror = function() {
        statusEl.textContent = 'Upload complete or connection reset. Reloading...';
        setTimeout(function() { location.reload(); }, 6000);
      };
      xhr.send(formData);
    });
  </script>
</body>
</html>
)rawliteral";
