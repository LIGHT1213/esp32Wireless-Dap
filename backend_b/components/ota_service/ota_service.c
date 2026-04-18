#include "ota_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "log_utils.h"
#include "wdap_http_ota.h"
#include "wifi_link.h"

static const char *TAG = "ota_service_b";

static const char *s_index_html =
    "<!doctype html>\n"
    "<html><head><meta charset='utf-8'>\n"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
    "<title>Wireless DAP OTA</title>\n"
    "<style>\n"
    ":root{color-scheme:light;background:#edf3ef;color:#163221;font-family:'Segoe UI',sans-serif;}\n"
    "body{margin:0;padding:24px;background:linear-gradient(160deg,#edf3ef 0%,#dbe7f5 100%);}\n"
    "h1{margin:0 0 12px;font-size:28px;}p{margin:0 0 18px;max-width:760px;line-height:1.5;}\n"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px;}\n"
    ".card{background:#ffffffcc;border:1px solid #bfd0c2;border-radius:18px;padding:18px;box-shadow:0 10px 25px rgba(22,50,33,.08);}\n"
    ".card h2{margin:0 0 12px;font-size:22px;}.status{min-height:64px;margin-bottom:12px;color:#254733;font-size:14px;line-height:1.45;}\n"
    "input[type=file]{display:block;width:100%;margin:10px 0 14px;}\n"
    "button{background:#1f6f4a;color:#fff;border:0;border-radius:999px;padding:12px 18px;font-size:14px;font-weight:700;cursor:pointer;}\n"
    "button:disabled{background:#8aa597;cursor:not-allowed;}.progress{height:10px;background:#d7e3dc;border-radius:999px;overflow:hidden;margin:14px 0;}\n"
    ".bar{height:100%;width:0;background:linear-gradient(90deg,#1f6f4a,#3fb47a);transition:width .15s ease;}\n"
    "pre{min-height:120px;margin:0;background:#102318;color:#d7fbe5;border-radius:12px;padding:12px;white-space:pre-wrap;word-break:break-word;font-size:12px;}\n"
    ".hint{margin-top:14px;font-size:12px;color:#4a6353;}\n"
    "</style></head><body>\n"
    "<h1>ESP32 Wireless DAP OTA</h1>\n"
    "<p>Connect the PC to the backend hotspot, open <strong>http://192.168.4.1/</strong>, and upload one raw <code>.bin</code> at a time. "
    "Do not start a second upload until the first device has rebooted.</p>\n"
    "<div class='grid'>\n"
    "<section class='card'><h2>backend_b</h2><div class='status' id='status-backend_b'>Detecting backend...</div>"
    "<input type='file' id='file-backend_b' accept='.bin,application/octet-stream'>"
    "<button id='btn-backend_b' onclick=\"uploadFirmware('backend_b')\">Upload backend_b.bin</button>"
    "<div class='progress'><div class='bar' id='bar-backend_b'></div></div>"
    "<pre id='log-backend_b'>Waiting for action.</pre>"
    "<div class='hint'>Uploading backend_b will reboot the hotspot device. The page will disconnect until the AP comes back.</div></section>\n"
    "<section class='card'><h2>frontend_a</h2><div class='status' id='status-frontend_a'>Waiting for frontend announce...</div>"
    "<input type='file' id='file-frontend_a' accept='.bin,application/octet-stream'>"
    "<button id='btn-frontend_a' onclick=\"uploadFirmware('frontend_a')\">Upload frontend_a.bin</button>"
    "<div class='progress'><div class='bar' id='bar-frontend_a'></div></div>"
    "<pre id='log-frontend_a'>Waiting for action.</pre>"
    "<div class='hint'>frontend_a is discovered over the existing UDP link. If it is missing, wait for Wi-Fi reconnection or refresh this page.</div></section>\n"
    "</div>\n"
    "<script>\n"
    "const deviceState={backend_b:{role:'backend_b',ip:location.hostname||'192.168.4.1',online:true},frontend_a:null};\n"
    "function $(id){return document.getElementById(id);}function setStatus(role,text){$('status-'+role).textContent=text;}\n"
    "function setLog(role,text){$('log-'+role).textContent=text;}function setProgress(role,value){$('bar-'+role).style.width=Math.max(0,Math.min(100,value))+'%';}\n"
    "function deviceBase(role){const info=deviceState[role];if(!info||!info.ip){return null;}return role==='backend_b'?'':'http://'+info.ip;}\n"
    "function updateCard(role,info){deviceState[role]=Object.assign({},deviceState[role]||{},info);const online=info.online===false?'offline':'online';"
    "const version=info.version||'unknown';const part=info.running_partition||'unknown';const ip=info.ip||'unknown';"
    "const busy=info.busy?'busy ('+(info.busy_reason||'busy')+')':'idle';setStatus(role,'IP: '+ip+' | '+online+' | version: '+version+' | partition: '+part+' | '+busy);"
    "const btn=$('btn-'+role);if(btn){btn.disabled=!info.online||!info.ip||info.busy;}}\n"
    "async function fetchInfo(role){const base=deviceBase(role);if(!base){updateCard(role,{role:role,online:false,ip:'',version:'not discovered',running_partition:'-',busy:false});return;}"
    "try{const resp=await fetch(base+'/api/info',{cache:'no-store'});if(!resp.ok){throw new Error('HTTP '+resp.status);}const info=await resp.json();info.online=true;updateCard(role,info);}"
    "catch(err){updateCard(role,{role:role,ip:(deviceState[role]&&deviceState[role].ip)||'',online:false,version:'unreachable',running_partition:'-',busy:false});setLog(role,'Info refresh failed: '+err.message);}}\n"
    "async function pollDevices(){try{const resp=await fetch('/api/devices',{cache:'no-store'});if(!resp.ok){throw new Error('HTTP '+resp.status);}const payload=await resp.json();"
    "const devices=Array.isArray(payload.devices)?payload.devices:[];const backend=devices.find(d=>d.role==='backend_b');const frontend=devices.find(d=>d.role==='frontend_a');"
    "if(backend){deviceState.backend_b=Object.assign({},deviceState.backend_b,backend);}if(frontend){deviceState.frontend_a=Object.assign({},deviceState.frontend_a||{},frontend);}else{deviceState.frontend_a={role:'frontend_a',ip:'',online:false};}"
    "await fetchInfo('backend_b');await fetchInfo('frontend_a');}catch(err){setStatus('backend_b','Device list error: '+err.message);setLog('backend_b','/api/devices failed: '+err.message);}}\n"
    "function validateFile(role){const input=$('file-'+role);if(!input.files||input.files.length===0){setLog(role,'Select a raw .bin file first.');return null;}const file=input.files[0];"
    "if(!file.name.toLowerCase().endsWith('.bin')){setLog(role,'Only raw .bin files are supported.');return null;}return file;}\n"
    "function uploadFirmware(role){const file=validateFile(role);if(!file){return;}const base=deviceBase(role);if(!base){setLog(role,'Device IP is not available yet.');return;}setProgress(role,0);"
    "setLog(role,'Uploading '+file.name+' ('+file.size+' bytes)...');$('btn-'+role).disabled=true;const xhr=new XMLHttpRequest();xhr.open('POST',base+'/api/ota',true);"
    "xhr.setRequestHeader('Content-Type','application/octet-stream');xhr.upload.onprogress=(event)=>{if(event.lengthComputable){setProgress(role,Math.round((event.loaded/event.total)*100));}};"
    "xhr.onload=()=>{let text=xhr.responseText;try{text=JSON.stringify(JSON.parse(xhr.responseText),null,2);}catch(err){}setLog(role,'HTTP '+xhr.status+'\\n'+text);"
    "if(role==='backend_b'&&xhr.status===200){setLog(role,text+'\\n\\nbackend_b is restarting. Reconnect to the hotspot and reopen http://192.168.4.1/.');}"
    "if(role==='frontend_a'&&xhr.status===200){setLog(role,text+'\\n\\nfrontend_a is restarting. It should reappear automatically after reconnect.');}setTimeout(pollDevices,2000);};"
    "xhr.onerror=()=>{setLog(role,'Upload failed due to a network error.');};xhr.onloadend=()=>{if(xhr.status!==200){$('btn-'+role).disabled=false;}};xhr.send(file);}\n"
    "pollDevices();setInterval(pollDevices,3000);\n"
    "</script></body></html>\n";

static esp_err_t append_json(char *buffer, size_t buffer_size, size_t *used, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buffer + *used, buffer_size - *used, fmt, args);
    va_end(args);

    if (written < 0) {
        return ESP_FAIL;
    }
    if ((size_t)written >= (buffer_size - *used)) {
        return ESP_ERR_INVALID_SIZE;
    }

    *used += (size_t)written;
    return ESP_OK;
}

static esp_err_t build_devices_json(char *buffer, size_t buffer_size, size_t *written_len)
{
    if (buffer == NULL || buffer_size == 0U || written_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char backend_ip[16] = "0.0.0.0";
    (void)wifi_link_get_local_ip_string(backend_ip, sizeof(backend_ip));

    const uint32_t now_ms = log_utils_uptime_ms();
    wifi_link_frontend_peer_info_t frontend = {0};
    (void)wifi_link_get_frontend_peer_info(&frontend);

    size_t used = 0U;
    ESP_RETURN_ON_ERROR(append_json(buffer,
                                    buffer_size,
                                    &used,
                                    "{\"devices\":[{\"role\":\"backend_b\",\"ip\":\"%s\",\"online\":true,\"last_seen_ms\":%" PRIu32 "}",
                                    backend_ip,
                                    now_ms),
                        TAG,
                        "append backend device failed");

    if (frontend.present) {
        ESP_RETURN_ON_ERROR(append_json(buffer,
                                        buffer_size,
                                        &used,
                                        ",{\"role\":\"frontend_a\",\"ip\":\"%s\",\"online\":%s,\"last_seen_ms\":%" PRIu32 "}",
                                        frontend.ip,
                                        frontend.online ? "true" : "false",
                                        frontend.last_seen_ms),
                            TAG,
                            "append frontend device failed");
    }

    ESP_RETURN_ON_ERROR(append_json(buffer, buffer_size, &used, "]}"), TAG, "append JSON trailer failed");
    *written_len = used;
    return ESP_OK;
}

esp_err_t ota_service_init(void)
{
    const wdap_http_ota_config_t config = {
        .get_ip = wifi_link_get_local_ip_string,
        .index_html = s_index_html,
        .build_devices_json = build_devices_json,
    };

    ESP_LOGI(TAG, "starting backend OTA portal");
    return wdap_http_ota_start(&config);
}
