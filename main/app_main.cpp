#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_check.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <mdns.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <app-common/zap-generated/cluster-enums.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <platform/AttributeList.h>
#include <platform/DeviceInfoProvider.h>

#include <cJSON.h>
#include <ds18b20.h>
#include <onewire_bus.h>

#include "ir_sender.h"

namespace {

static const char *kTag = "hvac_matter";
static const char *kNvsNamespace = "hvacmatter";
static const char *kNvsConfigKey = "config";
static const char *kWifiHostname = "ir-hvac";
static const char *kHttpMdnsInstanceName = "IR HVAC Setup";

static constexpr uint8_t kMaxHvacs = 2;
static constexpr uint8_t kMaxSensors = 4;
static constexpr size_t kMaxBodyBytes = 4096;
static constexpr uint16_t kCommissioningTimeoutSec = 300;
static constexpr int kDefaultEmitterGpio = 4;
static constexpr int kDefaultTempSensorGpio = 16;
static constexpr uint32_t kTempSensorPollMs = 5000;
static constexpr int16_t kMinSetpointCentiC = 1600;
static constexpr int16_t kMaxSetpointCentiC = 3000;
static constexpr uint8_t kProtocolModeDaikin = 0;
static constexpr uint8_t kProtocolModeGree = 1;
static constexpr uint8_t kProtocolModeMidea = 2;
static constexpr uint8_t kProtocolModeLg = 3;

extern const uint8_t kTlsRootCaPemStart[] asm("_binary_tls_root_ca_pem_start");
extern const uint8_t kTlsRootCaPemEnd[] asm("_binary_tls_root_ca_pem_end");

struct TempSensorConfig {
    char id[16];
    char name[32];
};

struct HvacConfig {
    char id[16];
    char protocol[24];
    int16_t model;
    int emitter_gpio;
    uint8_t temp_sensor_index;
    char current_temp_source[16];
};

struct AppConfig {
    uint8_t hvac_count;
    uint8_t temp_sensor_count;
    TempSensorConfig sensors[kMaxSensors];
    HvacConfig hvacs[kMaxHvacs];
};

struct HvacState {
    bool power;
    uint8_t system_mode;
    uint8_t fan_mode;
    int16_t cooling_setpoint;
    int16_t heating_setpoint;
    int16_t local_temperature;
    esp_err_t last_send_err;
};

AppConfig g_config = {};
HvacState g_states[kMaxHvacs] = {};
uint16_t g_endpoint_ids[kMaxHvacs] = {};
httpd_handle_t g_httpd = nullptr;
onewire_bus_handle_t g_onewire_bus = nullptr;
ds18b20_device_handle_t g_ds18b20 = nullptr;
TaskHandle_t g_temp_sensor_task = nullptr;
bool g_temp_sensor_detected = false;
float g_last_sensor_temp_c = 0.0f;
bool g_wifi_hostname_set = false;
bool g_mdns_started = false;
bool g_http_mdns_service_started = false;

using ModeSelectOption = chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;
using ModeSelectSemanticTag = chip::app::Clusters::ModeSelect::Structs::SemanticTagStruct::Type;
using ModeSelectStatus = chip::Protocols::InteractionModel::Status;

const ModeSelectSemanticTag kEmptyModeSelectSemanticTags[1] = {};
const ModeSelectOption kProtocolModeOptions[] = {
    {
        .label = chip::CharSpan::fromCharString("Daikin"),
        .mode = kProtocolModeDaikin,
        .semanticTags = chip::app::DataModel::List<const ModeSelectSemanticTag>(kEmptyModeSelectSemanticTags, 0),
    },
    {
        .label = chip::CharSpan::fromCharString("Gree"),
        .mode = kProtocolModeGree,
        .semanticTags = chip::app::DataModel::List<const ModeSelectSemanticTag>(kEmptyModeSelectSemanticTags, 0),
    },
    {
        .label = chip::CharSpan::fromCharString("Midea"),
        .mode = kProtocolModeMidea,
        .semanticTags = chip::app::DataModel::List<const ModeSelectSemanticTag>(kEmptyModeSelectSemanticTags, 0),
    },
    {
        .label = chip::CharSpan::fromCharString("LG"),
        .mode = kProtocolModeLg,
        .semanticTags = chip::app::DataModel::List<const ModeSelectSemanticTag>(kEmptyModeSelectSemanticTags, 0),
    },
};

void format_hvac_display_name(uint8_t hvac_index, char *buffer, size_t size)
{
    if (g_config.hvac_count <= 1) {
        snprintf(buffer, size, "%s", "Air Conditioner");
        return;
    }

    snprintf(buffer, size, "Air Conditioner %u", static_cast<unsigned>(hvac_index + 1));
}

void apply_hvac_user_labels()
{
    using chip::DeviceLayer::AttributeList;
    using chip::DeviceLayer::DeviceInfoProvider;
    using chip::DeviceLayer::GetDeviceInfoProvider;

    DeviceInfoProvider *provider = GetDeviceInfoProvider();
    if (!provider) {
        ESP_LOGW(kTag, "DeviceInfoProvider unavailable; skipping friendly endpoint labels");
        return;
    }

    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        if (g_endpoint_ids[i] == 0) {
            continue;
        }

        char display_name[32];
        format_hvac_display_name(i, display_name, sizeof(display_name));

        AttributeList<DeviceInfoProvider::UserLabelType, chip::DeviceLayer::kMaxUserLabelListLength> label_list;
        DeviceInfoProvider::UserLabelType name_label = {
            .label = chip::CharSpan::fromCharString("name"),
            .value = chip::CharSpan::fromCharString(display_name),
        };
        DeviceInfoProvider::UserLabelType role_label = {
            .label = chip::CharSpan::fromCharString("role"),
            .value = chip::CharSpan::fromCharString("Climate"),
        };
        if (label_list.add(name_label) != CHIP_NO_ERROR || label_list.add(role_label) != CHIP_NO_ERROR) {
            ESP_LOGW(kTag, "Failed to prepare friendly labels for endpoint %u", g_endpoint_ids[i]);
            continue;
        }

        CHIP_ERROR err = provider->SetUserLabelList(g_endpoint_ids[i], label_list);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGW(kTag, "Failed to apply friendly labels to endpoint %u: %" CHIP_ERROR_FORMAT,
                     g_endpoint_ids[i], err.Format());
            continue;
        }

        ESP_LOGI(kTag, "Applied friendly endpoint labels to endpoint %u (%s)", g_endpoint_ids[i], display_name);
    }
}

class ProtocolModeManager : public chip::app::Clusters::ModeSelect::SupportedModesManager
{
public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId) const override
    {
        return ModeOptionsProvider(kProtocolModeOptions,
                                   kProtocolModeOptions + (sizeof(kProtocolModeOptions) / sizeof(kProtocolModeOptions[0])));
    }

    ModeSelectStatus getModeOptionByMode(chip::EndpointId, uint8_t mode, const ModeSelectOption ** data_ptr) const override
    {
        for (const auto & option : kProtocolModeOptions) {
            if (option.mode == mode) {
                if (data_ptr) {
                    *data_ptr = &option;
                }
                return ModeSelectStatus::Success;
            }
        }
        return ModeSelectStatus::InvalidCommand;
    }
};

ProtocolModeManager g_protocol_mode_manager;

int16_t active_target_setpoint(const HvacState &state);
uint8_t normalized_fan_mode(uint8_t requested_mode, bool power);
uint8_t reported_fan_mode_for_state(const HvacState &state);
void prepare_wifi_hostname();
void set_wifi_hostname();
void ensure_mdns_started();
void advertise_http_setup_service_if_ready();
void sync_protocol_mode_to_matter(uint8_t index);
void advertise_http_setup_service(const esp_ip4_addr_t &ip);

static const char kSetupPageHtml[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Climate Setup</title>
<style>
:root{color-scheme:light;--ink:#111827;--muted:#697282;--line:rgba(17,24,39,.11);--card:rgba(255,255,255,.72);--blue:#007aff;--green:#34c759;--red:#ff3b30;--shadow:0 24px 80px rgba(31,41,55,.18)}
*{box-sizing:border-box}body{margin:0;min-height:100vh;font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display","SF Pro Text","Helvetica Neue",sans-serif;color:var(--ink);background:radial-gradient(circle at 18% 12%,#fff 0 16%,transparent 38%),radial-gradient(circle at 84% 10%,#dff3ff 0 14%,transparent 36%),linear-gradient(145deg,#eef5ff 0%,#f7f4ef 46%,#e9f7f1 100%);display:grid;place-items:center;padding:24px}
body:before{content:"";position:fixed;inset:0;background-image:linear-gradient(rgba(255,255,255,.38) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.3) 1px,transparent 1px);background-size:44px 44px;mask-image:linear-gradient(to bottom,rgba(0,0,0,.45),transparent 72%);pointer-events:none}
.shell{width:min(940px,100%);position:relative}.hero{display:grid;grid-template-columns:1.05fr .95fr;gap:22px;align-items:stretch}.panel,.preview{background:var(--card);border:1px solid rgba(255,255,255,.72);box-shadow:var(--shadow);backdrop-filter:blur(24px) saturate(1.2);-webkit-backdrop-filter:blur(24px) saturate(1.2);border-radius:34px}
.panel{padding:30px}.eyebrow{display:inline-flex;align-items:center;gap:8px;margin:0 0 18px;padding:7px 12px;border-radius:999px;background:rgba(255,255,255,.7);border:1px solid var(--line);font-size:13px;color:var(--muted);font-weight:650}.dot{width:8px;height:8px;border-radius:999px;background:var(--green);box-shadow:0 0 0 5px rgba(52,199,89,.13)}
h1{font-size:clamp(38px,7vw,72px);line-height:.92;letter-spacing:-.065em;margin:0 0 14px}.sub{font-size:18px;line-height:1.45;color:var(--muted);margin:0 0 28px;max-width:560px}.field{display:grid;gap:11px;margin:0 0 20px}.label{font-size:13px;text-transform:uppercase;letter-spacing:.12em;color:#8b95a5;font-weight:800}.select-wrap{position:relative}.select-wrap:after{content:"v";position:absolute;right:18px;top:50%;transform:translateY(-58%);font-size:18px;color:#7b8494;pointer-events:none}
select{appearance:none;width:100%;border:1px solid var(--line);background:rgba(255,255,255,.78);border-radius:22px;padding:18px 48px 18px 18px;font:700 20px/1 -apple-system,BlinkMacSystemFont,"SF Pro Display",sans-serif;color:var(--ink);outline:none;box-shadow:inset 0 1px 0 rgba(255,255,255,.8)}
.protocols{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin:10px 0 22px}.chip{border:1px solid var(--line);background:rgba(255,255,255,.68);border-radius:18px;padding:13px 10px;text-align:center;font-weight:760;color:#3f4652;cursor:pointer;transition:.18s ease}.chip.active{background:#111827;color:#fff;box-shadow:0 12px 30px rgba(17,24,39,.24);transform:translateY(-1px)}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:12px}button{border:0;border-radius:20px;padding:16px 18px;font-weight:800;font-size:16px;cursor:pointer;transition:.18s ease;box-shadow:0 12px 30px rgba(31,41,55,.12)}button:active{transform:scale(.98)}button:disabled{opacity:.55;cursor:not-allowed}.primary{background:var(--blue);color:white}.secondary{background:rgba(255,255,255,.75);color:#141a24;border:1px solid var(--line)}
.status{min-height:48px;margin-top:18px;padding:14px 16px;border-radius:18px;background:rgba(255,255,255,.58);border:1px solid var(--line);color:var(--muted);line-height:1.35}.status.good{color:#14783c;background:rgba(52,199,89,.12)}.status.bad{color:#b42318;background:rgba(255,59,48,.11)}
.preview{padding:24px;display:flex;flex-direction:column;justify-content:space-between;min-height:520px;overflow:hidden;position:relative}.preview:before{content:"";position:absolute;inset:auto -80px -140px auto;width:320px;height:320px;border-radius:50%;background:linear-gradient(135deg,#7dd3fc,#60a5fa 45%,#34d399);filter:blur(6px);opacity:.38}.device{position:relative;border-radius:32px;background:linear-gradient(180deg,#fdfdfd,#f2f5f8);border:1px solid rgba(255,255,255,.9);box-shadow:inset 0 1px 0 #fff,0 18px 42px rgba(17,24,39,.14);padding:20px;min-height:330px}.pill{display:inline-flex;gap:7px;align-items:center;border-radius:999px;background:#111827;color:white;padding:9px 12px;font-size:13px;font-weight:750}.temp{font-size:92px;letter-spacing:-.08em;margin:44px 0 0;font-weight:800}.deg{font-size:.38em;vertical-align:super;margin-left:2px}.meta{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:24px}.tile{background:rgba(255,255,255,.76);border:1px solid var(--line);border-radius:20px;padding:14px}.tile b{display:block;font-size:22px;letter-spacing:-.04em}.tile span{display:block;margin-top:4px;color:var(--muted);font-size:13px}.foot{position:relative;margin-top:18px;color:var(--muted);font-size:14px;line-height:1.45}
@media(max-width:760px){body{padding:14px;place-items:start center}.hero{grid-template-columns:1fr}.panel{padding:22px;border-radius:28px}.preview{min-height:auto;border-radius:28px}.protocols{grid-template-columns:repeat(2,1fr)}.actions{grid-template-columns:1fr}.device{min-height:260px}.temp{font-size:70px;margin-top:30px}}
</style>
</head>
<body>
<main class="shell">
<section class="hero">
<div class="panel">
<p class="eyebrow"><span class="dot"></span><span id="connectivity">Looking for device</span></p>
<h1>Set up your air conditioner</h1>
<p class="sub">Choose the IR protocol for this air conditioner, send a quick test, then save it to the device.</p>
<div class="field">
<span class="label">Air conditioner</span>
<div class="select-wrap"><select id="hvacSelect" aria-label="Air conditioner"></select></div>
</div>
<div class="field">
<span class="label">IR protocol</span>
<div class="protocols" id="protocols"></div>
</div>
<div class="actions">
<button class="secondary" id="testBtn">Test protocol</button>
<button class="primary" id="saveBtn">Save protocol</button>
</div>
<div class="status" id="status">Loading current configuration...</div>
</div>
<aside class="preview">
<div class="device">
<span class="pill">IR Climate Bridge</span>
<div class="temp"><span id="targetTemp">24</span><span class="deg">&deg;</span></div>
<div class="meta">
<div class="tile"><b id="currentProtocol">--</b><span>Selected protocol</span></div>
<div class="tile"><b id="emitterGpio">--</b><span>Emitter GPIO</span></div>
</div>
</div>
<p class="foot">Tip: if the AC beeps or reacts after testing, press Save. If nothing happens, try the next protocol.</p>
</aside>
</section>
</main>
<script>
const protocolLabels={daikin:"Daikin",gree:"Gree",midea:"Midea",lg:"LG"};
const els={connectivity:document.querySelector("#connectivity"),hvacSelect:document.querySelector("#hvacSelect"),protocols:document.querySelector("#protocols"),status:document.querySelector("#status"),testBtn:document.querySelector("#testBtn"),saveBtn:document.querySelector("#saveBtn"),currentProtocol:document.querySelector("#currentProtocol"),emitterGpio:document.querySelector("#emitterGpio"),targetTemp:document.querySelector("#targetTemp")};
let config=null,statusData=null,selectedId=null,selectedProtocol="daikin",busy=false,userEditing=false,lastStatusText="";
function setStatus(text,type=""){els.status.textContent=text;els.status.className=`status ${type}`;}
function currentHvac(){return (config?.hvacs||[]).find(h=>h.id===selectedId)||config?.hvacs?.[0];}
function statusHvac(){return (statusData?.hvacs||[]).find(h=>h.id===selectedId)||statusData?.hvacs?.[0];}
function applyRemoteState(cfg,stat,{announce=false}={}){
 config=cfg;statusData=stat;
 const hvac=currentHvac()||cfg?.hvacs?.[0];selectedId=selectedId||hvac?.id;
 const active=currentHvac();if(active&&!userEditing)selectedProtocol=(active.protocol||"daikin").toLowerCase();
 render();
 const label=protocolLabels[(active?.protocol||"").toLowerCase()]||active?.protocol;
 if(announce&&label){lastStatusText=`Current saved protocol is ${label}.`;setStatus(lastStatusText,"good");}
}
function render(){
 const hvacs=config?.hvacs||[];els.hvacSelect.innerHTML=hvacs.map(h=>`<option value="${h.id}">${h.id}</option>`).join("");
 if(!selectedId&&hvacs[0])selectedId=hvacs[0].id;els.hvacSelect.value=selectedId||"";
 const supported=statusData?.supported_protocols||["daikin","gree","midea","lg"];
 els.protocols.innerHTML=supported.map(p=>`<button class="chip ${p===selectedProtocol?"active":""}" data-protocol="${p}" type="button">${protocolLabels[p]||p}</button>`).join("");
 const hvac=currentHvac(), live=statusHvac();
 els.currentProtocol.textContent=protocolLabels[selectedProtocol]||selectedProtocol||"--";
 els.emitterGpio.textContent=hvac?`GPIO ${hvac.emitter_gpio}`:"--";
 els.targetTemp.textContent=Math.round(live?.cooling_setpoint_c||24);
 els.testBtn.disabled=!hvac;els.saveBtn.disabled=!hvac||selectedProtocol===hvac.protocol;
 els.connectivity.textContent=hvac?"Device connected":"No HVAC configured";
}
async function load(){
 try{
  const [cfg,stat]=await Promise.all([fetch("/api/config",{cache:"no-store"}),fetch("/api/status",{cache:"no-store"})]);
  applyRemoteState(await cfg.json(),await stat.json(),{announce:true});
 }catch(e){setStatus("Could not load the device setup page. Make sure you are on the same Wi-Fi network.","bad");}
}
async function refreshFromDevice(){
 if(busy||userEditing)return;
 try{
  const [cfg,stat]=await Promise.all([fetch("/api/config",{cache:"no-store"}),fetch("/api/status",{cache:"no-store"})]);
  applyRemoteState(await cfg.json(),await stat.json());
 }catch(e){}
}
async function testProtocol(){
 const hvac=currentHvac();if(!hvac)return;
 busy=true;setStatus(`Sending a ${protocolLabels[selectedProtocol]||selectedProtocol} test command...`);
 els.testBtn.disabled=true;
 try{
  const res=await fetch("/api/protocol/test",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({id:hvac.id,protocol:selectedProtocol})});
  const json=await res.json();if(!res.ok||!json.ok)throw new Error(json.error||"send_failed");
  setStatus(`Test sent with ${protocolLabels[selectedProtocol]||selectedProtocol}. If the AC reacted, save it.`,"good");
 }catch(e){setStatus(`Test failed: ${e.message}`,"bad");}
 finally{busy=false;render();}
}
async function saveProtocol(){
 const hvac=currentHvac();if(!hvac)return;
 busy=true;hvac.protocol=selectedProtocol;setStatus("Saving protocol to the device...");
 els.saveBtn.disabled=true;
 try{
  const res=await fetch("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(config)});
  const json=await res.json();if(!res.ok||!json.ok)throw new Error(json.error||"save_failed");
  await load();setStatus(`${protocolLabels[selectedProtocol]||selectedProtocol} saved. HomeKit commands will now use this protocol.`,"good");
 }catch(e){setStatus(`Save failed: ${e.message}`,"bad");}
 finally{busy=false;userEditing=false;render();}
}
els.hvacSelect.addEventListener("change",e=>{selectedId=e.target.value;userEditing=false;selectedProtocol=(currentHvac()?.protocol||"daikin").toLowerCase();render();});
els.protocols.addEventListener("click",e=>{const btn=e.target.closest("[data-protocol]");if(!btn)return;userEditing=true;selectedProtocol=btn.dataset.protocol;render();setStatus(`Ready to test ${protocolLabels[selectedProtocol]||selectedProtocol}.`);});
els.testBtn.addEventListener("click",testProtocol);els.saveBtn.addEventListener("click",saveProtocol);load();setInterval(refreshFromDevice,2500);
</script>
</body>
</html>)html";

void log_onboarding_codes()
{
    chip::PayloadContents payload;
    CHIP_ERROR err = GetPayloadContents(payload, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(kTag, "Failed to build onboarding payload: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    char code_buffer[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {};
    chip::MutableCharSpan manual_code(code_buffer);
    err = GetManualPairingCode(manual_code, payload);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(kTag, "Matter manual pairing code: %s", code_buffer);
    } else {
        ESP_LOGE(kTag, "Failed to generate manual pairing code: %" CHIP_ERROR_FORMAT, err.Format());
    }

    memset(code_buffer, 0, sizeof(code_buffer));
    chip::MutableCharSpan qr_code(code_buffer);
    err = GetQRCode(qr_code, payload);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(kTag, "Matter QR payload: %s", code_buffer);
    } else {
        ESP_LOGE(kTag, "Failed to generate Matter QR payload: %" CHIP_ERROR_FORMAT, err.Format());
    }

    ESP_LOGI(kTag, "Matter discriminator: %u", payload.discriminator.GetLongValue());
    ESP_LOGI(kTag, "Matter setup passcode: %u", static_cast<unsigned>(payload.setUpPINCode));
}

void log_embedded_tls_certificate()
{
    const size_t cert_len = static_cast<size_t>(kTlsRootCaPemEnd - kTlsRootCaPemStart);
    ESP_LOGI(kTag, "TLS root CA symbol range: start=%p end=%p", kTlsRootCaPemStart, kTlsRootCaPemEnd);
    if (cert_len == 0) {
        ESP_LOGW(kTag, "Embedded TLS root CA certificate is empty");
        return;
    }

    ESP_LOGI(kTag, "Embedded TLS root CA certificate ready (%u bytes, first_byte=0x%02x)",
             static_cast<unsigned>(cert_len), static_cast<unsigned>(kTlsRootCaPemStart[0]));
}

void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

void lowercase_copy(char *dst, size_t dst_size, const char *src)
{
    copy_string(dst, dst_size, src);
    for (size_t i = 0; dst[i] != '\0'; ++i) {
        dst[i] = static_cast<char>(tolower(static_cast<unsigned char>(dst[i])));
    }
}

int read_json_int(cJSON *object, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

const char *read_json_string(cJSON *object, const char *name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

uint8_t clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

int16_t clamp_setpoint(int value)
{
    if (value < kMinSetpointCentiC) {
        return kMinSetpointCentiC;
    }
    if (value > kMaxSetpointCentiC) {
        return kMaxSetpointCentiC;
    }
    return static_cast<int16_t>(((value + 50) / 100) * 100);
}

int16_t temperature_c_to_centi_c(float temperature_c)
{
    return static_cast<int16_t>(temperature_c * 100.0f);
}

int16_t fallback_local_temperature_for_state(const HvacState &state)
{
    return active_target_setpoint(state);
}

void refresh_local_temperature_from_source(uint8_t index)
{
    if (index >= g_config.hvac_count) {
        return;
    }

    if (g_temp_sensor_detected) {
        g_states[index].local_temperature = temperature_c_to_centi_c(g_last_sensor_temp_c);
        return;
    }

    g_states[index].local_temperature = fallback_local_temperature_for_state(g_states[index]);
}

void refresh_all_local_temperatures()
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        refresh_local_temperature_from_source(i);
    }
}

void report_local_temperature(uint8_t index)
{
    if (index >= g_config.hvac_count || g_endpoint_ids[index] == 0) {
        return;
    }

    esp_matter_attr_val_t local_temperature = esp_matter_nullable_int16(nullable<int16_t>(g_states[index].local_temperature));
    esp_matter::attribute::report(g_endpoint_ids[index], chip::app::Clusters::Thermostat::Id,
                                  chip::app::Clusters::Thermostat::Attributes::LocalTemperature::Id, &local_temperature);
}

void report_all_local_temperatures()
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        report_local_temperature(i);
    }
}

uint8_t protocol_to_mode(const char *protocol)
{
    if (!protocol) {
        return kProtocolModeDaikin;
    }
    if (strcmp(protocol, "gree") == 0) {
        return kProtocolModeGree;
    }
    if (strcmp(protocol, "lg") == 0) {
        return kProtocolModeLg;
    }
    if (strcmp(protocol, "midea") == 0) {
        return kProtocolModeMidea;
    }
    return kProtocolModeDaikin;
}

const char *mode_to_protocol(uint8_t mode)
{
    switch (mode) {
    case kProtocolModeGree:
        return "gree";
    case kProtocolModeLg:
        return "lg";
    case kProtocolModeMidea:
        return "midea";
    case kProtocolModeDaikin:
    default:
        return "daikin";
    }
}

void load_default_config()
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.hvac_count = 1;
    g_config.temp_sensor_count = 1;

    copy_string(g_config.sensors[0].id, sizeof(g_config.sensors[0].id), "sensor-1");
    copy_string(g_config.sensors[0].name, sizeof(g_config.sensors[0].name), "Ambient");

    copy_string(g_config.hvacs[0].id, sizeof(g_config.hvacs[0].id), "hvac-1");
    copy_string(g_config.hvacs[0].protocol, sizeof(g_config.hvacs[0].protocol), "daikin");
    g_config.hvacs[0].model = -1;
    g_config.hvacs[0].emitter_gpio = kDefaultEmitterGpio;
    g_config.hvacs[0].temp_sensor_index = 0;
    copy_string(g_config.hvacs[0].current_temp_source, sizeof(g_config.hvacs[0].current_temp_source), "sensor");
}

void load_default_states()
{
    for (uint8_t i = 0; i < kMaxHvacs; ++i) {
        g_states[i].power = false;
        g_states[i].system_mode = static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kCool);
        g_states[i].fan_mode = 5;
        g_states[i].cooling_setpoint = 2400;
        g_states[i].heating_setpoint = 2000;
        g_states[i].local_temperature = fallback_local_temperature_for_state(g_states[i]);
        g_states[i].last_send_err = ESP_OK;
    }
}

char *config_to_json()
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "hvac_count", g_config.hvac_count);

    cJSON *sensors = cJSON_AddArrayToObject(root, "temp_sensors");
    for (uint8_t i = 0; i < g_config.temp_sensor_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", g_config.sensors[i].id);
        cJSON_AddStringToObject(item, "name", g_config.sensors[i].name);
        cJSON_AddItemToArray(sensors, item);
    }

    cJSON *hvacs = cJSON_AddArrayToObject(root, "hvacs");
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", g_config.hvacs[i].id);
        cJSON_AddStringToObject(item, "protocol", g_config.hvacs[i].protocol);
        cJSON_AddNumberToObject(item, "model", g_config.hvacs[i].model);
        cJSON_AddNumberToObject(item, "emitter_gpio", g_config.hvacs[i].emitter_gpio);
        cJSON_AddNumberToObject(item, "temp_sensor_index", g_config.hvacs[i].temp_sensor_index);
        cJSON_AddStringToObject(item, "current_temp_source", g_config.hvacs[i].current_temp_source);
        cJSON_AddBoolToObject(item, "protocol_supported", ir_sender_is_protocol_supported(g_config.hvacs[i].protocol));
        cJSON_AddItemToArray(hvacs, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

bool parse_config_json(const char *json, AppConfig *out)
{
    if (!json || !out) {
        return false;
    }

    AppConfig next = {};
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }

    cJSON *hvacs = cJSON_GetObjectItemCaseSensitive(root, "hvacs");
    cJSON *sensors = cJSON_GetObjectItemCaseSensitive(root, "temp_sensors");
    if (!cJSON_IsArray(hvacs)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, sensors) {
        if (next.temp_sensor_count >= kMaxSensors) {
            break;
        }
        TempSensorConfig &sensor = next.sensors[next.temp_sensor_count++];
        copy_string(sensor.id, sizeof(sensor.id), read_json_string(item, "id"));
        copy_string(sensor.name, sizeof(sensor.name), read_json_string(item, "name"));
    }

    cJSON_ArrayForEach(item, hvacs) {
        if (next.hvac_count >= kMaxHvacs) {
            break;
        }
        HvacConfig &hvac = next.hvacs[next.hvac_count++];
        copy_string(hvac.id, sizeof(hvac.id), read_json_string(item, "id"));
        lowercase_copy(hvac.protocol, sizeof(hvac.protocol), read_json_string(item, "protocol"));
        hvac.model = static_cast<int16_t>(read_json_int(item, "model", -1));
        hvac.emitter_gpio = read_json_int(item, "emitter_gpio", kDefaultEmitterGpio);
        hvac.temp_sensor_index =
            clamp_u8(static_cast<uint8_t>(read_json_int(item, "temp_sensor_index", 0)), 0, kMaxSensors - 1);
        lowercase_copy(hvac.current_temp_source, sizeof(hvac.current_temp_source), read_json_string(item, "current_temp_source"));
        if (hvac.current_temp_source[0] == '\0') {
            copy_string(hvac.current_temp_source, sizeof(hvac.current_temp_source), "sensor");
        }
        if (hvac.protocol[0] == '\0') {
            copy_string(hvac.protocol, sizeof(hvac.protocol), "unknown");
        }
        if (hvac.id[0] == '\0') {
            snprintf(hvac.id, sizeof(hvac.id), "hvac-%u", next.hvac_count);
        }
    }

    if (next.hvac_count == 0) {
        cJSON_Delete(root);
        return false;
    }
    if (next.temp_sensor_count == 0) {
        next.temp_sensor_count = 1;
        copy_string(next.sensors[0].id, sizeof(next.sensors[0].id), "sensor-1");
        copy_string(next.sensors[0].name, sizeof(next.sensors[0].name), "Ambient");
    }

    *out = next;
    cJSON_Delete(root);
    return true;
}

esp_err_t save_config()
{
    char *json = config_to_json();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kNvsConfigKey, json);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    cJSON_free(json);
    return err;
}

void load_config()
{
    load_default_config();
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    size_t size = 0;
    esp_err_t err = nvs_get_str(handle, kNvsConfigKey, nullptr, &size);
    if (err != ESP_OK || size < 2) {
        nvs_close(handle);
        return;
    }

    char *json = static_cast<char *>(calloc(1, size));
    if (!json) {
        nvs_close(handle);
        return;
    }

    err = nvs_get_str(handle, kNvsConfigKey, json, &size);
    if (err == ESP_OK) {
        AppConfig next = {};
        if (parse_config_json(json, &next)) {
            g_config = next;
        }
    }

    free(json);
    nvs_close(handle);
}

int endpoint_index_from_id(uint16_t endpoint_id)
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        if (g_endpoint_ids[i] == endpoint_id) {
            return i;
        }
    }
    return -1;
}

int hvac_index_from_id(const char *id)
{
    if (!id) {
        return -1;
    }
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        if (strcmp(g_config.hvacs[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int16_t active_target_setpoint(const HvacState &state)
{
    switch (state.system_mode) {
    case static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kHeat):
    case static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kEmergencyHeat):
        return state.heating_setpoint;
    default:
        return state.cooling_setpoint;
    }
}

uint8_t normalize_system_mode_for_power(uint8_t requested_mode, bool power)
{
    using chip::app::Clusters::Thermostat::SystemModeEnum;

    if (!power) {
        return static_cast<uint8_t>(SystemModeEnum::kOff);
    }

    if (requested_mode == static_cast<uint8_t>(SystemModeEnum::kOff)) {
        return static_cast<uint8_t>(SystemModeEnum::kCool);
    }

    return requested_mode;
}

uint8_t normalized_fan_mode(uint8_t requested_mode, bool power)
{
    using chip::app::Clusters::FanControl::FanModeEnum;

    switch (requested_mode) {
    case static_cast<uint8_t>(FanModeEnum::kOn):
        return static_cast<uint8_t>(FanModeEnum::kHigh);
    case static_cast<uint8_t>(FanModeEnum::kSmart):
        return static_cast<uint8_t>(FanModeEnum::kAuto);
    case static_cast<uint8_t>(FanModeEnum::kOff):
        return power ? static_cast<uint8_t>(FanModeEnum::kAuto) : static_cast<uint8_t>(FanModeEnum::kOff);
    default:
        return requested_mode;
    }
}

uint8_t reported_fan_mode_for_state(const HvacState &state)
{
    using chip::app::Clusters::FanControl::FanModeEnum;
    using chip::app::Clusters::Thermostat::SystemModeEnum;

    if (!state.power || state.system_mode == static_cast<uint8_t>(SystemModeEnum::kOff)) {
        return static_cast<uint8_t>(FanModeEnum::kOff);
    }

    return normalized_fan_mode(state.fan_mode, true);
}

uint8_t thermostat_running_mode_for_state(const HvacState &state)
{
    using chip::app::Clusters::Thermostat::SystemModeEnum;
    using chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;

    if (!state.power || state.system_mode == static_cast<uint8_t>(SystemModeEnum::kOff)) {
        return static_cast<uint8_t>(ThermostatRunningModeEnum::kOff);
    }

    if (state.system_mode == static_cast<uint8_t>(SystemModeEnum::kHeat) ||
        state.system_mode == static_cast<uint8_t>(SystemModeEnum::kEmergencyHeat)) {
        return static_cast<uint8_t>(ThermostatRunningModeEnum::kHeat);
    }

    return static_cast<uint8_t>(ThermostatRunningModeEnum::kCool);
}

uint16_t thermostat_running_state_for_state(const HvacState &state)
{
    using chip::app::Clusters::Thermostat::RelayStateBitmap;
    using chip::app::Clusters::Thermostat::SystemModeEnum;

    if (!state.power || state.system_mode == static_cast<uint8_t>(SystemModeEnum::kOff)) {
        return 0;
    }

    if (state.system_mode == static_cast<uint8_t>(SystemModeEnum::kHeat) ||
        state.system_mode == static_cast<uint8_t>(SystemModeEnum::kEmergencyHeat)) {
        return static_cast<uint16_t>(RelayStateBitmap::kHeat) | static_cast<uint16_t>(RelayStateBitmap::kFan);
    }

    return static_cast<uint16_t>(RelayStateBitmap::kCool) | static_cast<uint16_t>(RelayStateBitmap::kFan);
}

void sync_linked_matter_state(uint16_t endpoint_id, const HvacState &state, bool report_on_off, bool report_system_mode)
{
    if (endpoint_id == 0) {
        return;
    }

    if (report_on_off) {
        esp_matter_attr_val_t onoff = esp_matter_bool(state.power);
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::OnOff::Id,
                                      chip::app::Clusters::OnOff::Attributes::OnOff::Id, &onoff);
    }

    if (report_system_mode) {
        esp_matter_attr_val_t mode = esp_matter_enum8(state.system_mode);
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                      chip::app::Clusters::Thermostat::Attributes::SystemMode::Id, &mode);
    }

    if (esp_matter::attribute::get(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                   chip::app::Clusters::Thermostat::Attributes::ThermostatRunningMode::Id)) {
        esp_matter_attr_val_t running_mode = esp_matter_enum8(thermostat_running_mode_for_state(state));
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                      chip::app::Clusters::Thermostat::Attributes::ThermostatRunningMode::Id,
                                      &running_mode);
    }

    if (esp_matter::attribute::get(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                   chip::app::Clusters::Thermostat::Attributes::ThermostatRunningState::Id)) {
        esp_matter_attr_val_t running_state = esp_matter_bitmap16(thermostat_running_state_for_state(state));
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                      chip::app::Clusters::Thermostat::Attributes::ThermostatRunningState::Id,
                                      &running_state);
    }

    if (esp_matter::attribute::get(endpoint_id, chip::app::Clusters::FanControl::Id,
                                   chip::app::Clusters::FanControl::Attributes::FanMode::Id)) {
        esp_matter_attr_val_t fan_mode = esp_matter_enum8(reported_fan_mode_for_state(state));
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::FanControl::Id,
                                      chip::app::Clusters::FanControl::Attributes::FanMode::Id, &fan_mode);
    }
}

void sync_protocol_mode_to_matter(uint8_t index)
{
    if (index >= g_config.hvac_count) {
        return;
    }

    const uint16_t endpoint_id = g_endpoint_ids[index];
    if (endpoint_id == 0) {
        return;
    }

    esp_matter_attr_val_t protocol_mode = esp_matter_uint8(protocol_to_mode(g_config.hvacs[index].protocol));
    const esp_err_t err = esp_matter::attribute::update(endpoint_id, chip::app::Clusters::ModeSelect::Id,
                                                        chip::app::Clusters::ModeSelect::Attributes::CurrentMode::Id,
                                                        &protocol_mode);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to sync protocol mode for %s endpoint=%u to Matter: %s", g_config.hvacs[index].id, endpoint_id,
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(kTag, "Synced protocol mode for %s endpoint=%u to %s", g_config.hvacs[index].id, endpoint_id,
             g_config.hvacs[index].protocol);
}

esp_err_t dispatch_hvac_ir_with_options(uint8_t index, const char *reason, const char *protocol_override,
                                        bool force_power_on)
{
    if (index >= g_config.hvac_count) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *protocol = protocol_override && protocol_override[0] != '\0' ? protocol_override : g_config.hvacs[index].protocol;
    hvac_ir_command_t command = {
        .protocol = protocol,
        .model = g_config.hvacs[index].model,
        .emitter_gpio = g_config.hvacs[index].emitter_gpio,
        .power = force_power_on ? true : g_states[index].power,
        .system_mode = g_states[index].system_mode,
        .fan_mode = g_states[index].fan_mode,
        .target_temperature_centi_c = active_target_setpoint(g_states[index]),
    };

    const esp_err_t err = ir_sender_send(&command);
    g_states[index].last_send_err = err;
    ESP_LOGI(kTag,
             "send reason=%s hvac=%s endpoint=%u protocol=%s emitter_gpio=%d power=%d mode=%u cool=%.2f heat=%.2f fan=%u err=%s",
             reason ? reason : "unknown", g_config.hvacs[index].id, g_endpoint_ids[index], protocol,
             g_config.hvacs[index].emitter_gpio, command.power, g_states[index].system_mode,
             g_states[index].cooling_setpoint / 100.0, g_states[index].heating_setpoint / 100.0, g_states[index].fan_mode,
             esp_err_to_name(err));
    return err;
}

esp_err_t dispatch_hvac_ir(uint8_t index, const char *reason)
{
    return dispatch_hvac_ir_with_options(index, reason, nullptr, false);
}

esp_err_t send_json_response(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json ? json : "{}");
}

esp_err_t setup_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kSetupPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t config_get_handler(httpd_req_t *req)
{
    char *json = config_to_json();
    esp_err_t err = send_json_response(req, json);
    if (json) {
        cJSON_free(json);
    }
    return err;
}

esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "hvac_count", g_config.hvac_count);

    cJSON *protocols = cJSON_AddArrayToObject(root, "supported_protocols");
    cJSON_AddItemToArray(protocols, cJSON_CreateString(mode_to_protocol(kProtocolModeDaikin)));
    cJSON_AddItemToArray(protocols, cJSON_CreateString(mode_to_protocol(kProtocolModeGree)));
    cJSON_AddItemToArray(protocols, cJSON_CreateString(mode_to_protocol(kProtocolModeMidea)));
    cJSON_AddItemToArray(protocols, cJSON_CreateString(mode_to_protocol(kProtocolModeLg)));

    cJSON *hvacs = cJSON_AddArrayToObject(root, "hvacs");
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", g_config.hvacs[i].id);
        cJSON_AddNumberToObject(item, "endpoint_id", g_endpoint_ids[i]);
        cJSON_AddStringToObject(item, "protocol", g_config.hvacs[i].protocol);
        cJSON_AddNumberToObject(item, "model", g_config.hvacs[i].model);
        cJSON_AddNumberToObject(item, "emitter_gpio", g_config.hvacs[i].emitter_gpio);
        cJSON_AddBoolToObject(item, "protocol_supported", ir_sender_is_protocol_supported(g_config.hvacs[i].protocol));
        cJSON_AddBoolToObject(item, "power", g_states[i].power);
        cJSON_AddNumberToObject(item, "system_mode", g_states[i].system_mode);
        cJSON_AddNumberToObject(item, "fan_mode", g_states[i].fan_mode);
        cJSON_AddNumberToObject(item, "cooling_setpoint_c", g_states[i].cooling_setpoint / 100);
        cJSON_AddNumberToObject(item, "heating_setpoint_c", g_states[i].heating_setpoint / 100);
        cJSON_AddNumberToObject(item, "local_temperature_c", g_states[i].local_temperature / 100.0);
        cJSON_AddStringToObject(item, "current_temp_source", g_config.hvacs[i].current_temp_source);
        cJSON_AddNumberToObject(item, "temp_sensor_index", g_config.hvacs[i].temp_sensor_index);
        cJSON_AddNumberToObject(item, "temp_sensor_gpio", kDefaultTempSensorGpio);
        cJSON_AddBoolToObject(item, "temp_sensor_detected", g_temp_sensor_detected);
        cJSON_AddStringToObject(item, "last_send_error", esp_err_to_name(g_states[i].last_send_err));
        cJSON_AddItemToArray(hvacs, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t err = send_json_response(req, json);
    if (json) {
        cJSON_free(json);
    }
    return err;
}

esp_err_t config_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodyBytes)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_body\"}");
    }

    char *body = static_cast<char *>(calloc(1, req->content_len + 1));
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    }

    int offset = 0;
    while (offset < req->content_len) {
        const int read = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (read <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"recv_failed\"}");
        }
        offset += read;
    }

    AppConfig next = {};
    if (!parse_config_json(body, &next)) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
    }

    const AppConfig previous = g_config;
    g_config = next;
    esp_err_t err = save_config();
    free(body);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"save_failed\"}");
    }

    const uint8_t sync_count = previous.hvac_count < g_config.hvac_count ? previous.hvac_count : g_config.hvac_count;
    for (uint8_t i = 0; i < sync_count; ++i) {
        if (strcmp(previous.hvacs[i].protocol, g_config.hvacs[i].protocol) != 0) {
            sync_protocol_mode_to_matter(i);
        }
    }

    return httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":false}");
}

esp_err_t hvac_send_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodyBytes)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_body\"}");
    }

    char *body = static_cast<char *>(calloc(1, req->content_len + 1));
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    }

    int offset = 0;
    while (offset < req->content_len) {
        const int read = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (read <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"recv_failed\"}");
        }
        offset += read;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
    }

    const int index = hvac_index_from_id(read_json_string(root, "id"));
    if (index < 0) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"error\":\"unknown_hvac\"}");
    }

    HvacState next = g_states[index];
    cJSON *item = nullptr;

    item = cJSON_GetObjectItemCaseSensitive(root, "power");
    if (cJSON_IsBool(item)) {
        next.power = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "system_mode");
    if (cJSON_IsNumber(item)) {
        next.system_mode = static_cast<uint8_t>(item->valueint);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "fan_mode");
    if (cJSON_IsNumber(item)) {
        next.fan_mode = static_cast<uint8_t>(item->valueint);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "cooling_setpoint_centi_c");
    if (cJSON_IsNumber(item)) {
        next.cooling_setpoint = clamp_setpoint(item->valueint);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "heating_setpoint_centi_c");
    if (cJSON_IsNumber(item)) {
        next.heating_setpoint = clamp_setpoint(item->valueint);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "target_temperature_centi_c");
    if (cJSON_IsNumber(item)) {
        const int16_t target = clamp_setpoint(item->valueint);
        if (next.system_mode == static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kHeat) ||
            next.system_mode == static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kEmergencyHeat)) {
            next.heating_setpoint = target;
        } else {
            next.cooling_setpoint = target;
        }
    }

    g_states[index] = next;
    const esp_err_t err = dispatch_hvac_ir(static_cast<uint8_t>(index), "http");
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "id", g_config.hvacs[index].id);
    cJSON_AddStringToObject(resp, "protocol", g_config.hvacs[index].protocol);
    cJSON_AddNumberToObject(resp, "emitter_gpio", g_config.hvacs[index].emitter_gpio);
    cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    cJSON_AddNumberToObject(resp, "active_target_c", active_target_setpoint(g_states[index]) / 100);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    esp_err_t send_err = send_json_response(req, json);
    if (json) {
        cJSON_free(json);
    }
    return send_err;
}

esp_err_t protocol_test_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodyBytes)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_body\"}");
    }

    char *body = static_cast<char *>(calloc(1, req->content_len + 1));
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    }

    int offset = 0;
    while (offset < req->content_len) {
        const int read = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (read <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"recv_failed\"}");
        }
        offset += read;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
    }

    const int index = hvac_index_from_id(read_json_string(root, "id"));
    char protocol[24] = {};
    lowercase_copy(protocol, sizeof(protocol), read_json_string(root, "protocol"));

    if (index < 0) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"error\":\"unknown_hvac\"}");
    }
    if (!ir_sender_is_protocol_supported(protocol)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"unsupported_protocol\"}");
    }

    const esp_err_t err = dispatch_hvac_ir_with_options(static_cast<uint8_t>(index), "protocol-test", protocol, true);
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "id", g_config.hvacs[index].id);
    cJSON_AddStringToObject(resp, "protocol", protocol);
    cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    esp_err_t send_err = send_json_response(req, json);
    if (json) {
        cJSON_free(json);
    }
    return send_err;
}

const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
        return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED:
        return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED:
        return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

void log_forwarded_esp_system_event(const ChipDeviceEvent *event)
{
    if (!event) {
        return;
    }

    const auto &esp_event = event->Platform.ESPSystemEvent;

    if (esp_event.Base == WIFI_EVENT) {
        switch (esp_event.Id) {
        case WIFI_EVENT_STA_START:
            set_wifi_hostname();
            ESP_LOGI(kTag, "WiFi diag: station started");
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const auto &wifi_event = esp_event.Data.WiFiStaConnected;
            ESP_LOGI(kTag, "WiFi diag: connected ssid=%.*s channel=%u authmode=%u",
                     wifi_event.ssid_len, reinterpret_cast<const char *>(wifi_event.ssid), wifi_event.channel,
                     static_cast<unsigned>(wifi_event.authmode));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const auto &wifi_event = esp_event.Data.WiFiStaDisconnected;
            const uint8_t reason = wifi_event.reason;
            ESP_LOGW(kTag, "WiFi diag: disconnected ssid=%.*s reason=%u (%s) rssi=%d",
                     wifi_event.ssid_len, reinterpret_cast<const char *>(wifi_event.ssid), static_cast<unsigned>(reason),
                     wifi_disconnect_reason_name(reason), wifi_event.rssi);
            break;
        }
        default:
            ESP_LOGI(kTag, "WiFi diag: WIFI_EVENT id=%ld", static_cast<long>(esp_event.Id));
            break;
        }
        return;
    }

    if (esp_event.Base == IP_EVENT) {
        switch (esp_event.Id) {
        case IP_EVENT_STA_GOT_IP: {
            const auto &ip_event = esp_event.Data.IpGotIp;
            ESP_LOGI(kTag, "WiFi diag: got IPv4 " IPSTR " gw=" IPSTR, IP2STR(&ip_event.ip_info.ip),
                     IP2STR(&ip_event.ip_info.gw));
            advertise_http_setup_service(ip_event.ip_info.ip);
            break;
        }
        case IP_EVENT_STA_LOST_IP:
            ESP_LOGW(kTag, "WiFi diag: lost IPv4 address");
            break;
        default:
            ESP_LOGI(kTag, "WiFi diag: IP_EVENT id=%ld", static_cast<long>(esp_event.Id));
            break;
        }
    }
}

void start_http_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;

    if (httpd_start(&g_httpd, &config) != ESP_OK) {
        ESP_LOGW(kTag, "HTTP server start failed");
        return;
    }

    httpd_uri_t setup_page = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = setup_page_get_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &setup_page);

    httpd_uri_t setup_alias = {
        .uri = "/setup",
        .method = HTTP_GET,
        .handler = setup_page_get_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &setup_alias);

    httpd_uri_t config_get = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &config_get);

    httpd_uri_t config_post = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &config_post);

    httpd_uri_t status_get = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &status_get);

    httpd_uri_t hvac_send_post = {
        .uri = "/api/hvac/send",
        .method = HTTP_POST,
        .handler = hvac_send_post_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &hvac_send_post);

    httpd_uri_t protocol_test_post = {
        .uri = "/api/protocol/test",
        .method = HTTP_POST,
        .handler = protocol_test_post_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &protocol_test_post);
}

void set_wifi_hostname()
{
    if (g_wifi_hostname_set) {
        return;
    }

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        ESP_LOGW(kTag, "WiFi station netif unavailable; hostname %s not set yet", kWifiHostname);
        return;
    }

    const esp_err_t err = esp_netif_set_hostname(sta_netif, kWifiHostname);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to set WiFi hostname %s: %s", kWifiHostname, esp_err_to_name(err));
        return;
    }

    g_wifi_hostname_set = true;
    ESP_LOGI(kTag, "WiFi hostname set to %s; try http://%s or router-provided local DNS", kWifiHostname,
             kWifiHostname);
}

void prepare_wifi_hostname()
{
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Failed to create default event loop before hostname setup: %s", esp_err_to_name(err));
        return;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Failed to initialize esp-netif before hostname setup: %s", esp_err_to_name(err));
        return;
    }

    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") && !esp_netif_create_default_wifi_sta()) {
        ESP_LOGW(kTag, "Failed to create WiFi station netif before hostname setup");
        return;
    }

    set_wifi_hostname();
}

void ensure_mdns_started()
{
    if (g_mdns_started) {
        return;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Failed to initialize mDNS responder: %s", esp_err_to_name(err));
        return;
    }

    err = mdns_hostname_set(kWifiHostname);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to set mDNS hostname %s.local: %s", kWifiHostname, esp_err_to_name(err));
        return;
    }

    err = mdns_instance_name_set(kHttpMdnsInstanceName);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to set mDNS instance name %s: %s", kHttpMdnsInstanceName, esp_err_to_name(err));
        return;
    }

    g_mdns_started = true;
    ESP_LOGI(kTag, "mDNS responder ready for %s.local", kWifiHostname);
}

void advertise_http_setup_service(const esp_ip4_addr_t &ip)
{
    ensure_mdns_started();
    if (!g_mdns_started) {
        return;
    }

    if (!g_http_mdns_service_started) {
        esp_err_t err = ESP_OK;
        mdns_txt_item_t service_txt[] = {
            {"path", "/"},
            {"app", "ir-hvac"},
        };

        err = mdns_service_add(kHttpMdnsInstanceName, "_http", "_tcp", 80, service_txt,
                               sizeof(service_txt) / sizeof(service_txt[0]));
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Failed to advertise HTTP setup service over mDNS: %s", esp_err_to_name(err));
            return;
        }
        g_http_mdns_service_started = true;
    }

    ESP_LOGI(kTag, "HTTP setup mDNS ready at http://%s.local/ (" IPSTR ", %s._http._tcp.local)", kWifiHostname,
             IP2STR(&ip), kHttpMdnsInstanceName);
}

void advertise_http_setup_service_if_ready()
{
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        ESP_LOGW(kTag, "WiFi station netif unavailable; skipping immediate mDNS advertisement");
        return;
    }

    esp_netif_ip_info_t ip_info = {};
    const esp_err_t err = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to read WiFi station IP for mDNS advertisement: %s", esp_err_to_name(err));
        return;
    }

    if (ip_info.ip.addr == 0) {
        ESP_LOGI(kTag, "WiFi station has no IPv4 address yet; waiting for IP event before advertising mDNS");
        return;
    }

    advertise_http_setup_service(ip_info.ip);
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(kTag, "Matter commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(kTag, "Commissioning window opened");
        log_onboarding_codes();
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(kTag, "Commissioning window closed");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(kTag, "Fabric removed");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            auto &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!mgr.IsCommissioningWindowOpen()) {
                mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(kCommissioningTimeoutSec),
                                                 chip::CommissioningWindowAdvertisement::kDnssdOnly);
            }
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
        log_forwarded_esp_system_event(event);
        break;
    default:
        break;
    }
}

static esp_err_t app_identification_cb(esp_matter::identification::callback_type_t, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *)
{
    ESP_LOGI(kTag, "Identify endpoint=%u effect=%u variant=%u", endpoint_id, effect_id, effect_variant);
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *)
{
    if (type != esp_matter::attribute::PRE_UPDATE) {
        return ESP_OK;
    }

    const int index = endpoint_index_from_id(endpoint_id);
    if (index < 0) {
        return ESP_OK;
    }

    HvacState next = g_states[index];

    if (cluster_id == chip::app::Clusters::OnOff::Id &&
        attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id) {
        next.power = val->val.b;
        next.system_mode = normalize_system_mode_for_power(next.system_mode, next.power);
        next.fan_mode = normalized_fan_mode(next.fan_mode, next.power);
    } else if (cluster_id == chip::app::Clusters::Thermostat::Id &&
               attribute_id == chip::app::Clusters::Thermostat::Attributes::SystemMode::Id) {
        next.system_mode = static_cast<uint8_t>(val->val.u8);
        next.power = next.system_mode != static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kOff);
        next.system_mode = normalize_system_mode_for_power(next.system_mode, next.power);
        next.fan_mode = normalized_fan_mode(next.fan_mode, next.power);
    } else if (cluster_id == chip::app::Clusters::Thermostat::Id &&
               attribute_id == chip::app::Clusters::Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
        next.cooling_setpoint = clamp_setpoint(val->val.i16);
        val->val.i16 = next.cooling_setpoint;
    } else if (cluster_id == chip::app::Clusters::Thermostat::Id &&
               attribute_id == chip::app::Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
        next.heating_setpoint = clamp_setpoint(val->val.i16);
        val->val.i16 = next.heating_setpoint;
    } else if (cluster_id == chip::app::Clusters::FanControl::Id &&
               attribute_id == chip::app::Clusters::FanControl::Attributes::FanMode::Id) {
        next.fan_mode = normalized_fan_mode(static_cast<uint8_t>(val->val.u8), next.power);
        val->val.u8 = next.fan_mode;
    } else if (cluster_id == chip::app::Clusters::ModeSelect::Id &&
               attribute_id == chip::app::Clusters::ModeSelect::Attributes::CurrentMode::Id) {
        copy_string(g_config.hvacs[index].protocol, sizeof(g_config.hvacs[index].protocol), mode_to_protocol(val->val.u8));
        ESP_RETURN_ON_ERROR(save_config(), kTag, "Failed to persist protocol selection for %s", g_config.hvacs[index].id);
        ESP_LOGI(kTag, "Protocol mode changed for %s endpoint=%u mode=%u protocol=%s", g_config.hvacs[index].id, endpoint_id,
                 val->val.u8, g_config.hvacs[index].protocol);
        return ESP_OK;
    } else {
        return ESP_OK;
    }

    g_states[index] = next;
    refresh_local_temperature_from_source(static_cast<uint8_t>(index));
    sync_linked_matter_state(endpoint_id, next,
                             cluster_id == chip::app::Clusters::Thermostat::Id &&
                                 attribute_id == chip::app::Clusters::Thermostat::Attributes::SystemMode::Id,
                             cluster_id == chip::app::Clusters::OnOff::Id &&
                                 attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id);
    report_local_temperature(static_cast<uint8_t>(index));
    const esp_err_t ir_err = dispatch_hvac_ir(static_cast<uint8_t>(index), "matter");
    if (ir_err != ESP_OK) {
        ESP_LOGW(kTag, "Matter state accepted, but IR dispatch failed for endpoint %u: %s", endpoint_id,
                 esp_err_to_name(ir_err));
    }
    return ESP_OK;
}

void temperature_sensor_task(void *)
{
    while (true) {
        bool detected = false;
        float temperature_c = 0.0f;

        if (g_ds18b20) {
            if (ds18b20_trigger_temperature_conversion(g_ds18b20) == ESP_OK &&
                ds18b20_get_temperature(g_ds18b20, &temperature_c) == ESP_OK) {
                detected = true;
                g_last_sensor_temp_c = temperature_c;
            }
        }

        const bool sensor_state_changed = (g_temp_sensor_detected != detected);
        g_temp_sensor_detected = detected;
        refresh_all_local_temperatures();
        report_all_local_temperatures();

        if (g_temp_sensor_detected) {
            ESP_LOGI(kTag, "DS18B20 temperature=%.2fC gpio=%d", g_last_sensor_temp_c, kDefaultTempSensorGpio);
        } else if (sensor_state_changed) {
            ESP_LOGW(kTag, "No DS18B20 detected on GPIO %d, using setpoint fallback for local temperature", kDefaultTempSensorGpio);
        }

        vTaskDelay(pdMS_TO_TICKS(kTempSensorPollMs));
    }
}

void init_temperature_sensor()
{
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = kDefaultTempSensorGpio,
        .flags = {
            .en_pull_up = true,
        },
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,
    };

    esp_err_t err = onewire_new_bus_rmt(&bus_config, &rmt_config, &g_onewire_bus);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to initialize 1-Wire bus on GPIO %d: %s", kDefaultTempSensorGpio, esp_err_to_name(err));
        return;
    }

    onewire_device_iter_handle_t iter = nullptr;
    err = onewire_new_device_iter(g_onewire_bus, &iter);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to create DS18B20 iterator on GPIO %d: %s", kDefaultTempSensorGpio, esp_err_to_name(err));
        onewire_bus_del(g_onewire_bus);
        g_onewire_bus = nullptr;
        return;
    }

    onewire_device_t device;
    err = onewire_device_iter_get_next(iter, &device);
    if (err == ESP_OK) {
        ds18b20_config_t ds18b20_config = {};
        err = ds18b20_new_device(&device, &ds18b20_config, &g_ds18b20);
        if (err == ESP_OK) {
            ds18b20_set_resolution(g_ds18b20, DS18B20_RESOLUTION_12B);
            ESP_LOGI(kTag, "DS18B20 detected on GPIO %d address=%016llX", kDefaultTempSensorGpio, device.address);
        } else {
            ESP_LOGW(kTag, "1-Wire device on GPIO %d is not a DS18B20: %s", kDefaultTempSensorGpio, esp_err_to_name(err));
        }
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "No DS18B20 detected on GPIO %d", kDefaultTempSensorGpio);
    } else {
        ESP_LOGW(kTag, "DS18B20 search failed on GPIO %d: %s", kDefaultTempSensorGpio, esp_err_to_name(err));
    }

    if (iter) {
        onewire_del_device_iter(iter);
    }

    if (!g_ds18b20) {
        onewire_bus_del(g_onewire_bus);
        g_onewire_bus = nullptr;
        ESP_LOGI(kTag, "Released 1-Wire bus on GPIO %d because no DS18B20 is active", kDefaultTempSensorGpio);
        return;
    }

    xTaskCreate(temperature_sensor_task, "temp_sensor", 4096, nullptr, 5, &g_temp_sensor_task);
}

void create_hvac_endpoints(esp_matter::node_t *node)
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        esp_matter::endpoint::room_air_conditioner::config_t config = {};
        config.on_off.on_off = g_states[i].power;
        config.thermostat.local_temperature = g_states[i].local_temperature;
        config.thermostat.features.cooling.occupied_cooling_setpoint = g_states[i].cooling_setpoint;
        config.thermostat.features.heating.occupied_heating_setpoint = g_states[i].heating_setpoint;
        config.thermostat.control_sequence_of_operation =
            static_cast<uint8_t>(chip::app::Clusters::Thermostat::ControlSequenceOfOperationEnum::kCoolingAndHeating);
        config.thermostat.feature_flags |= esp_matter::cluster::thermostat::feature::cooling::get_id();
        config.thermostat.feature_flags |= esp_matter::cluster::thermostat::feature::heating::get_id();
        config.thermostat.system_mode = g_states[i].system_mode;

        esp_matter::endpoint_t *endpoint =
            esp_matter::endpoint::room_air_conditioner::create(node, &config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
        if (!endpoint) {
            ESP_LOGE(kTag, "Failed to create HVAC endpoint %u", i);
            continue;
        }

        esp_matter::cluster::fan_control::config_t fan_control_config = {};
        fan_control_config.fan_mode = reported_fan_mode_for_state(g_states[i]);
        fan_control_config.fan_mode_sequence =
            static_cast<uint8_t>(chip::app::Clusters::FanControl::FanModeSequenceEnum::kOffLowMedHighAuto);
        fan_control_config.percent_setting = nullable<uint8_t>();
        fan_control_config.percent_current = 0;
        esp_matter::cluster_t *fan_cluster =
            esp_matter::cluster::fan_control::create(endpoint, &fan_control_config, esp_matter::CLUSTER_FLAG_SERVER);
        if (!fan_cluster) {
            ESP_LOGW(kTag, "Failed to create Fan Control cluster for endpoint %u", esp_matter::endpoint::get_id(endpoint));
        } else {
            esp_matter::cluster::fan_control::feature::fan_auto::add(fan_cluster);
        }

        esp_matter::cluster::user_label::config_t user_label_config = {};
        if (!esp_matter::cluster::user_label::create(endpoint, &user_label_config, esp_matter::CLUSTER_FLAG_SERVER)) {
            ESP_LOGW(kTag, "Failed to create User Label cluster for endpoint %u", esp_matter::endpoint::get_id(endpoint));
        }

        esp_matter::cluster::mode_select::config_t mode_select_config = {};
        copy_string(mode_select_config.mode_select_description, sizeof(mode_select_config.mode_select_description), "IR Protocol");
        mode_select_config.current_mode = protocol_to_mode(g_config.hvacs[i].protocol);
        mode_select_config.delegate = &g_protocol_mode_manager;
        if (!esp_matter::cluster::mode_select::create(endpoint, &mode_select_config, esp_matter::CLUSTER_FLAG_SERVER)) {
            ESP_LOGW(kTag, "Failed to create Mode Select cluster for endpoint %u", esp_matter::endpoint::get_id(endpoint));
        }

        g_endpoint_ids[i] = esp_matter::endpoint::get_id(endpoint);
        ESP_LOGI(kTag, "Created HVAC endpoint %u for %s using protocol=%s emitter_gpio=%d", g_endpoint_ids[i],
                 g_config.hvacs[i].id, g_config.hvacs[i].protocol, g_config.hvacs[i].emitter_gpio);
    }
}

}  // namespace

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    log_embedded_tls_certificate();

    load_default_states();
    load_config();
    refresh_all_local_temperatures();

    esp_matter::node::config_t node_config;
    esp_matter::node_t *node = esp_matter::node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ESP_ERROR_CHECK(node ? ESP_OK : ESP_FAIL);

    create_hvac_endpoints(node);

    prepare_wifi_hostname();

    ESP_LOGI(kTag, "Starting Matter stack");
    err = esp_matter::start(app_event_cb);
    ESP_ERROR_CHECK(err);
    ESP_LOGI(kTag, "Matter stack started");

    apply_hvac_user_labels();
    init_temperature_sensor();

    log_onboarding_codes();

    start_http_server();
    advertise_http_setup_service_if_ready();
    ESP_LOGI(kTag, "HTTP setup UI ready at / with APIs /api/config, /api/status, /api/hvac/send, and /api/protocol/test");
    ESP_LOGI(kTag, "GPIO selection: ir_emitter=%d temp_sensor=%d", g_config.hvacs[0].emitter_gpio, kDefaultTempSensorGpio);
}
