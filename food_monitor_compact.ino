#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>
#include <RTClib.h>
#include <WiFi.h>
#include <PubSubClient.h>
#define sensor_t camera_sensor_t
#include "esp_camera.h"
#undef sensor_t
#include "img_converters.h"

constexpr uint16_t CONFIG_VERSION=1, DEFAULT_MQTT_PORT=1883, DEFAULT_LOG_INTERVAL_MIN=30, DEFAULT_PHOTO_INTERVAL_MIN=30;
constexpr char DEFAULT_DEVICE_NAME[]="Food Monitor", DEFAULT_WIFI_SSID[]="", DEFAULT_WIFI_PASSWORD[]="", DEFAULT_MQTT_SERVER[]="192.168.50.250", DEFAULT_TRIAL_NAME[]="Trial";
constexpr uint32_t LIVE_REFRESH_MS=2000, GRAPH_INTERVAL_MS=5000, SD_CHECK_MS=10000, SD_RECOVERY_CHECK_MS=5000, SD_INIT_HZ=1000000, SD_RECOVERY_HZ=400000;
constexpr uint8_t SD_RECOVERY_TRIES=3, HIST=64, GAS_BASELINE_SAMPLES=10, ROTATION=3;
constexpr float TEMP_GRAPH_MIN=10, TEMP_GRAPH_MAX=40, HUM_GRAPH_MIN=0, HUM_GRAPH_MAX=100, PRESS_GRAPH_MIN=950, PRESS_GRAPH_MAX=1050, LIGHT_GRAPH_MIN=0, LIGHT_GRAPH_MAX=5000;
constexpr float GAS_GRAPH_MIN_RATIO=0.8f, GAS_GRAPH_BASE_MAX_RATIO=6.0f, GAS_CAUTION_RATIO=3.5f, GAS_DANGER_RATIO=5.0f, GAS_BASELINE_STABILITY_FRAC=0.05f;
constexpr uint32_t GAS_WARMUP_MS=10UL*60UL*1000UL, GAS_BASELINE_SAMPLE_MS=30000, LONG_PRESS_MS=2000, BOTH_HOLD_MS=5000, DEBOUNCE_MS=40, WIFI_RETRY_MS=10000, MQTT_RETRY_MS=5000;
const char *MQTT_USER="", *MQTT_PASSWORD="";
struct AppConfig {char deviceName[32], wifiSsid[33], wifiPassword[65], mqttServer[64]; uint16_t mqttPort; char trialName[32]; uint16_t logIntervalMin, photoIntervalMin;};
AppConfig cfg;
unsigned long logIntervalMs(){return (unsigned long)cfg.logIntervalMin*60000UL;}
unsigned long photoIntervalMs(){return (unsigned long)cfg.photoIntervalMin*60000UL;}
const char *TOPIC_STATUS="foodmonitor/status", *TOPIC_TELEMETRY="foodmonitor/telemetry", *TOPIC_PHOTO="foodmonitor/photo", *TOPIC_CMD_MEASURE="foodmonitor/command/measure", *TOPIC_CMD_PHOTO="foodmonitor/command/photo";
constexpr int SPI_SCK=14, SPI_MOSI=15, SPI_MISO=2, TFT_CS=0, TFT_DC=1, TFT_RST=-1, SD_CS=33;
constexpr int I2C_SDA=26, I2C_SCL=27, MCP_ADDR=0x20, MQ135_PIN=32;
constexpr int MCP_ACTION=0, MCP_PAGE=1, MCP_PHOTO_LED=2;
constexpr int CAM_XCLK=21, CAM_SIOD=26, CAM_SIOC=27, CAM_Y9=35, CAM_Y8=34, CAM_Y7=39, CAM_Y6=36, CAM_Y5=19, CAM_Y4=18, CAM_Y3=5, CAM_Y2=4, CAM_VSYNC=25, CAM_HREF=23, CAM_PCLK=22, CAM_PWDN=-1, CAM_RESET=-1;
constexpr uint16_t C_BG=ST77XX_BLACK, C_HDR=0x2124, C_GRID=0x31A6, C_DIM=0x8410;

Adafruit_ST7735 tft(&SPI, TFT_CS, TFT_DC, TFT_RST); Adafruit_MCP23X17 mcp; Adafruit_BME280 bme; BH1750 lightMeter; RTC_DS3231 rtc;
WiFiClient wifiClient; PubSubClient mqttClient(wifiClient);
bool mcpReady=false, bmeReady=false, bhReady=false, rtcReady=false, cameraReady=false, sdReady=false, mqttReady=false;
bool mqttMeasureRequest=false, mqttPhotoRequest=false; unsigned long lastWifiAttempt=0, lastMqttAttempt=0, lastPhoto=0;
struct Metric { const char* shortName; const char* unit; uint8_t dec; uint16_t color; };
const Metric M[]={{"Temp","C",1,ST77XX_RED},{"Humid","%",1,ST77XX_CYAN},{"Press","hPa",1,ST77XX_GREEN},{"Light","lux",0,ST77XX_YELLOW},{"Gas","raw",0,ST77XX_MAGENTA}};
const uint8_t NM=sizeof(M)/sizeof(M[0]), NPAGES=NM+1;
float hist[NM][HIST], currentValue[NM]={NAN,NAN,NAN,NAN,NAN}; uint8_t head=0, count=0, page=0;
unsigned long lastLog=0, lastLive=0, lastGraph=0, lastSDCheck=0;
bool gasCalibrated=false; uint8_t gasBaselineCount=0, gasBaselineHead=0; float gasBaselineSamples[GAS_BASELINE_SAMPLES];
float gasBaseline=NAN, currentGasRatio=NAN, gasGraphCeiling=GAS_GRAPH_BASE_MAX_RATIO; unsigned long gasWarmupStarted=0, lastGasBaselineSample=0;
bool prevAction=HIGH, prevPage=HIGH, bothGestureActive=false, ignoreButtonsUntilReleased=false; unsigned long actionDownAt=0, pageDownAt=0, bothDownAt=0;

void deselectSD(){digitalWrite(SD_CS,HIGH);}

void deselectTFT(){digitalWrite(TFT_CS,HIGH);}

float medianSamples(const float *src,uint8_t n){
  float a[GAS_BASELINE_SAMPLES];
  for(uint8_t i=0;i<n;i++) a[i]=src[i];
  for(uint8_t i=1;i<n;i++){
    float x=a[i]; int8_t j=i-1;
    while(j>=0 && a[j]>x){a[j+1]=a[j];j--;}
    a[j+1]=x;}
  if(n&1) return a[n/2];
  return (a[n/2-1]+a[n/2])*0.5f;}

bool gasBaselineWindowStable(float &medianOut){
  if(gasBaselineCount<GAS_BASELINE_SAMPLES) return false;
  medianOut=medianSamples(gasBaselineSamples,GAS_BASELINE_SAMPLES);
  if(!isfinite(medianOut) || medianOut<=0) return false;
  float lo=INFINITY, hi=-INFINITY;
  for(uint8_t i=0;i<GAS_BASELINE_SAMPLES;i++){
    lo=min(lo,gasBaselineSamples[i]); hi=max(hi,gasBaselineSamples[i]);}
  return (hi-lo)<=medianOut*GAS_BASELINE_STABILITY_FRAC;}

void updateGasCalibration(float raw){
  if(!isfinite(raw) || raw<=0) return;
  if(gasCalibrated){
    currentGasRatio=raw/gasBaseline;
    if(currentGasRatio>gasGraphCeiling) gasGraphCeiling=ceilf(currentGasRatio/2.0f)*2.0f;
    return;}
  currentGasRatio=NAN; unsigned long now=millis();
  if(now-gasWarmupStarted<GAS_WARMUP_MS) return;
  if(gasBaselineCount==0 || now-lastGasBaselineSample>=GAS_BASELINE_SAMPLE_MS){
    lastGasBaselineSample=now; gasBaselineSamples[gasBaselineHead]=raw; gasBaselineHead=(gasBaselineHead+1)%GAS_BASELINE_SAMPLES;
    if(gasBaselineCount<GAS_BASELINE_SAMPLES) gasBaselineCount++;
    float med=NAN;
    if(gasBaselineWindowStable(med)){
      gasBaseline=med; gasCalibrated=true; currentGasRatio=raw/gasBaseline; gasGraphCeiling=GAS_GRAPH_BASE_MAX_RATIO;
      if(currentGasRatio>gasGraphCeiling) gasGraphCeiling=ceilf(currentGasRatio/2.0f)*2.0f;}}}

void readSensors(float v[]) {
  v[0] = bmeReady ? bme.readTemperature() : NAN; v[1] = bmeReady ? bme.readHumidity() : NAN; v[2] = bmeReady ? bme.readPressure()/100.0f : NAN;
  v[3] = bhReady ? lightMeter.readLightLevel() : NAN; v[4] = (float)analogRead(MQ135_PIN); updateGasCalibration(v[4]);
  for(uint8_t i=0;i<NM;i++) currentValue[i]=v[i];}

void pushHistory(const float v[]) {
  for(uint8_t m=0;m<NM;m++){
    if(m==4) hist[m][head]=(gasCalibrated && isfinite(v[4]) && gasBaseline>0)?v[4]/gasBaseline:NAN;
    else hist[m][head]=v[m];}
  head=(head+1)%HIST;
  if(count<HIST) count++;}

float histAt(uint8_t m,uint8_t k){ return hist[m][(head+HIST-count+k)%HIST]; }

bool minMax(uint8_t m,float &lo,float &hi){
  bool any=false; lo=INFINITY; hi=-INFINITY;
  for(uint8_t k=0;k<count;k++){
    float v=histAt(m,k);
    if(!isfinite(v)) continue;
    if(v<lo) lo=v;
    if(v>hi) hi=v;
    any=true;}
  return any;}

float gasGraphMaxRatio(){return max(gasGraphCeiling,GAS_GRAPH_BASE_MAX_RATIO);}

void plotRange(uint8_t m,float &lo,float &hi){
  switch(m){
    case 0: lo=TEMP_GRAPH_MIN; hi=TEMP_GRAPH_MAX; break; case 1: lo=HUM_GRAPH_MIN; hi=HUM_GRAPH_MAX; break; case 2: lo=PRESS_GRAPH_MIN; hi=PRESS_GRAPH_MAX; break;
    case 3: lo=LIGHT_GRAPH_MIN; hi=LIGHT_GRAPH_MAX; break;
    case 4:
      lo=GAS_GRAPH_MIN_RATIO; hi=gasGraphMaxRatio();
      break;
    default: lo=0.0f; hi=1.0f; break;}}

void printValue(float v,uint8_t dec){ if(isfinite(v)) tft.print(v,dec); else tft.print("N/A"); }
const char* gasRiskText();
String jsonFloat(float v,uint8_t decimals){
  if(!isfinite(v)) return String("null");
  char buf[32]; snprintf(buf,sizeof(buf),"%.*f",(int)decimals,(double)v);
  return String(buf);}

void publishHomeAssistantDiscovery(){
  if(!mqttClient.connected()) return;
  struct HASensor { const char* id; const char* name; const char* key; const char* unit; const char* devClass; };
  const HASensor sensors[]={
    {"temperature","Temperature","temperature","°C","temperature"},
    {"humidity","Humidity","humidity","%","humidity"},
    {"pressure","Pressure","pressure","hPa","pressure"},
    {"light","Light","light","lx","illuminance"},
    {"gas_raw","Gas raw","gas_raw","",""},
    {"gas_base","Gas base","gas_base","",""},
    {"gas_increase","Gas increase","gas_increase","x",""}}; char topic[128], payload[768];
  for(const auto &x:sensors){
    snprintf(topic,sizeof(topic),"homeassistant/sensor/foodmonitor_%s/config",x.id); String extra;
    if(x.devClass[0]) extra=String("\"device_class\":\"")+x.devClass+"\",";
    snprintf(payload,sizeof(payload),
      "{\"name\":\"%s\",\"unique_id\":\"foodmonitor_%s\",\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.%s }}\",\"unit_of_measurement\":\"%s\",\"availability_topic\":\"%s\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\",%s\"device\":{\"identifiers\":[\"foodmonitor\"],\"name\":\"%s\",\"model\":\"ENG30002 prototype\"}}",
      x.name,x.id,TOPIC_TELEMETRY,x.key,x.unit,TOPIC_STATUS,extra.c_str(),cfg.deviceName);
    mqttClient.publish(topic,payload,true);}
  snprintf(topic,sizeof(topic),"homeassistant/sensor/foodmonitor_risk/config");
  snprintf(payload,sizeof(payload),
    "{\"name\":\"Gas risk band\",\"unique_id\":\"foodmonitor_risk\",\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.risk }}\",\"availability_topic\":\"%s\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\",\"device\":{\"identifiers\":[\"foodmonitor\"],\"name\":\"%s\"}}",
    TOPIC_TELEMETRY,TOPIC_STATUS,cfg.deviceName);
  mqttClient.publish(topic,payload,true); snprintf(topic,sizeof(topic),"homeassistant/button/foodmonitor_measure/config");
  snprintf(payload,sizeof(payload),
    "{\"name\":\"Take measurement\",\"unique_id\":\"foodmonitor_measure\",\"command_topic\":\"%s\",\"payload_press\":\"measure\",\"availability_topic\":\"%s\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\",\"device\":{\"identifiers\":[\"foodmonitor\"],\"name\":\"%s\"}}",
    TOPIC_CMD_MEASURE,TOPIC_STATUS,cfg.deviceName);
  mqttClient.publish(topic,payload,true); snprintf(topic,sizeof(topic),"homeassistant/button/foodmonitor_photo/config");
  snprintf(payload,sizeof(payload),
    "{\"name\":\"Take photo\",\"unique_id\":\"foodmonitor_photo\",\"command_topic\":\"%s\",\"payload_press\":\"photo\",\"availability_topic\":\"%s\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\",\"device\":{\"identifiers\":[\"foodmonitor\"],\"name\":\"%s\"}}",
    TOPIC_CMD_PHOTO,TOPIC_STATUS,cfg.deviceName);
  mqttClient.publish(topic,payload,true);}

void mqttCallback(char* topic,byte* payload,unsigned int length){
  char msg[24]; unsigned int n=min(length,(unsigned int)(sizeof(msg)-1));
  for(unsigned int i=0;i<n;i++) msg[i]=(char)payload[i]; msg[n]='\0';
  String v(msg); v.trim(); v.toLowerCase(); bool active=(v=="1"||v=="on"||v=="true"||v=="measure"||v=="photo"||v=="go");
  if(!active) return;
  if(strcmp(topic,TOPIC_CMD_MEASURE)==0) mqttMeasureRequest=true;
  else if(strcmp(topic,TOPIC_CMD_PHOTO)==0) mqttPhotoRequest=true;}

void startWiFi(){
  if(!cfg.wifiSsid[0]) return;
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.persistent(false); WiFi.begin(cfg.wifiSsid,cfg.wifiPassword); lastWifiAttempt=millis();}

bool connectMQTT(){
  if(WiFi.status()!=WL_CONNECTED || !cfg.mqttServer[0]){ mqttReady=false; return false; }
  uint64_t mac=ESP.getEfuseMac(); char clientId[40]; snprintf(clientId,sizeof(clientId),"foodmonitor-%04X",(uint16_t)(mac&0xFFFF)); bool ok;
  if(MQTT_USER[0]) ok=mqttClient.connect(clientId,MQTT_USER,MQTT_PASSWORD,TOPIC_STATUS,0,true,"offline");
  else ok=mqttClient.connect(clientId,TOPIC_STATUS,0,true,"offline");
  mqttReady=ok;
  if(ok){mqttClient.publish(TOPIC_STATUS,"online",true); mqttClient.subscribe(TOPIC_CMD_MEASURE); mqttClient.subscribe(TOPIC_CMD_PHOTO); publishHomeAssistantDiscovery();}
  return ok;}

void serviceNetwork(bool buttonsIdle){
  unsigned long now=millis();
  if(!cfg.wifiSsid[0] || !cfg.mqttServer[0]){ mqttReady=false; return; }
  if(WiFi.status()!=WL_CONNECTED){
    mqttReady=false;
    if(buttonsIdle && now-lastWifiAttempt>=WIFI_RETRY_MS){lastWifiAttempt=now; WiFi.disconnect(); WiFi.begin(cfg.wifiSsid,cfg.wifiPassword);}
    return;}
  if(mqttClient.connected()){
    mqttReady=true; mqttClient.loop(); return;}
  mqttReady=false;
  if(buttonsIdle && now-lastMqttAttempt>=MQTT_RETRY_MS){
    lastMqttAttempt=now; connectMQTT();}}

bool publishTelemetry(const float v[],const char* source){
  if(!mqttClient.connected()) return false;
  String payload="{"; payload += "\"temperature\":"; payload += jsonFloat(v[0],2); payload += ",\"humidity\":"; payload += jsonFloat(v[1],2);
  payload += ",\"pressure\":"; payload += jsonFloat(v[2],2); payload += ",\"light\":"; payload += jsonFloat(v[3],1);
  payload += ",\"gas_raw\":"; payload += jsonFloat(v[4],0); payload += ",\"gas_base\":"; payload += gasCalibrated?jsonFloat(gasBaseline,0):String("null");
  payload += ",\"gas_increase\":"; payload += gasCalibrated?jsonFloat(v[4]/gasBaseline,3):String("null");
  payload += ",\"risk\":\""; payload += gasRiskText(); payload += "\""; payload += ",\"sd\":"; payload += sdReady?"true":"false";
  payload += ",\"rssi\":"; payload += String(WiFi.RSSI()); payload += ",\"trial\":\""; payload += cfg.trialName; payload += "\"";
  payload += ",\"source\":\""; payload += source; payload += "\"}";
  return mqttClient.publish(TOPIC_TELEMETRY,payload.c_str(),true);}

void publishPhotoEvent(const char* filename,size_t bytes){
  if(!mqttClient.connected()) return;
  char payload[180]; snprintf(payload,sizeof(payload),"{\"file\":\"%s\",\"bytes\":%u,\"risk\":\"%s\",\"trial\":\"%s\"}",filename,(unsigned int)bytes,gasRiskText(),cfg.trialName);
  mqttClient.publish(TOPIC_PHOTO,payload,true);}

void resetSharedSPI(){
  SD.end(); deselectTFT(); deselectSD(); delay(5); SPI.end(); delay(5); SPI.begin(SPI_SCK,SPI_MISO,SPI_MOSI,-1); deselectTFT(); deselectSD(); delay(100);}

bool beginSD(uint32_t hz){
  SD.end(); deselectTFT(); deselectSD(); delay(5);
  if(!SD.begin(SD_CS,SPI,hz)){
    SD.end(); deselectSD();
    return false;}
  if(SD.cardType()==CARD_NONE){
    SD.end(); deselectSD();
    return false;}
  return true;}

bool mountSD(){
  if(beginSD(SD_INIT_HZ)){ sdReady=true; return true; }
  for(uint8_t attempt=0;attempt<SD_RECOVERY_TRIES;attempt++){
    resetSharedSPI();
    if(beginSD(SD_RECOVERY_HZ)){ sdReady=true; return true; }
    delay(100);}
  sdReady=false;
  return false;}

void unmountSD(){ SD.end(); deselectSD(); delay(2); }
bool testSD(){
  if(!mountSD()) return false;
  SD.remove("/BOOTTEST.TXT"); File f=SD.open("/BOOTTEST.TXT",FILE_WRITE);
  if(!f){unmountSD(); sdReady=false; return false;}
  size_t n=f.println("Food Monitor SD test"); f.close(); SD.remove("/BOOTTEST.TXT"); unmountSD(); sdReady=n>0;
  return sdReady;}

bool checkSDPresent(){
  resetSharedSPI(); bool ok=beginSD(SD_RECOVERY_HZ);
  if(ok) unmountSD();
  else {SD.end(); deselectSD();}
  sdReady=ok;
  return ok;}

void applyDefaultConfig(AppConfig &c){
  memset(&c,0,sizeof(c)); strlcpy(c.deviceName,DEFAULT_DEVICE_NAME,sizeof(c.deviceName)); strlcpy(c.wifiSsid,DEFAULT_WIFI_SSID,sizeof(c.wifiSsid));
  strlcpy(c.wifiPassword,DEFAULT_WIFI_PASSWORD,sizeof(c.wifiPassword)); strlcpy(c.mqttServer,DEFAULT_MQTT_SERVER,sizeof(c.mqttServer)); c.mqttPort=DEFAULT_MQTT_PORT;
  strlcpy(c.trialName,DEFAULT_TRIAL_NAME,sizeof(c.trialName)); c.logIntervalMin=DEFAULT_LOG_INTERVAL_MIN; c.photoIntervalMin=DEFAULT_PHOTO_INTERVAL_MIN;}

bool validLabel(const char* s,size_t maxLen){
  size_t n=strlen(s); if(n==0||n>maxLen) return false;
  for(size_t i=0;i<n;i++){
    char c=s[i];
    if(c<32||c>126||c=='"'||c=='\\'||c==','||c=='=') return false;}
  return true;}

bool validateConfig(const AppConfig &c){
  if(!validLabel(c.deviceName,sizeof(c.deviceName)-1)) return false;
  if(!validLabel(c.trialName,sizeof(c.trialName)-1)) return false;
  if(strlen(c.wifiSsid)>32||strlen(c.wifiPassword)>64||strlen(c.mqttServer)>63) return false;
  if(c.mqttPort==0) return false;
  if(c.logIntervalMin<1||c.logIntervalMin>1440) return false;
  if(c.photoIntervalMin<1||c.photoIntervalMin>1440) return false;
  return true;}

bool parseUIntValue(const String &s,uint32_t minV,uint32_t maxV,uint32_t &outVal){
  String t=s; t.trim(); if(!t.length()) return false;
  char* end=nullptr; unsigned long v=strtoul(t.c_str(),&end,10);
  if(!end||*end!='\0'||v<minV||v>maxV) return false;
  outVal=(uint32_t)v; return true;}

bool copyConfigValue(char* dst,size_t dstSize,const String &value){
  if(value.length()>=dstSize) return false;
  value.toCharArray(dst,dstSize); return true;}

bool loadConfigFromSD(AppConfig &loaded){
  if(!mountSD()) return false;
  if(!SD.exists("/config.txt")){ unmountSD(); return false; }
  File f=SD.open("/config.txt",FILE_READ);
  if(!f){ unmountSD(); return false; }
  AppConfig c={}; uint16_t seen=0; bool ok=true;
  while(f.available()&&ok){
    String line=f.readStringUntil('\n');
    if(line.endsWith("\r")) line.remove(line.length()-1);
    String trimmed=line; trimmed.trim();
    if(!trimmed.length()||trimmed.startsWith("#")) continue;
    int eq=line.indexOf('='); if(eq<=0){ok=false;break;}
    String key=line.substring(0,eq); key.trim(); String value=line.substring(eq+1); uint16_t bit=0; uint32_t n=0;
    if(key=="CONFIG_VERSION"){
      bit=1u<<0; if(!parseUIntValue(value,CONFIG_VERSION,CONFIG_VERSION,n)) ok=false;
    }else if(key=="DEVICE_NAME"){
      bit=1u<<1; if(!copyConfigValue(c.deviceName,sizeof(c.deviceName),value)) ok=false;
    }else if(key=="WIFI_SSID"){
      bit=1u<<2; if(!copyConfigValue(c.wifiSsid,sizeof(c.wifiSsid),value)) ok=false;
    }else if(key=="WIFI_PASSWORD"){
      bit=1u<<3; if(!copyConfigValue(c.wifiPassword,sizeof(c.wifiPassword),value)) ok=false;
    }else if(key=="MQTT_SERVER"){
      bit=1u<<4; if(!copyConfigValue(c.mqttServer,sizeof(c.mqttServer),value)) ok=false;
    }else if(key=="MQTT_PORT"){
      bit=1u<<5; if(!parseUIntValue(value,1,65535,n)) ok=false; else c.mqttPort=(uint16_t)n;
    }else if(key=="TRIAL_NAME"){
      bit=1u<<6; if(!copyConfigValue(c.trialName,sizeof(c.trialName),value)) ok=false;
    }else if(key=="LOG_INTERVAL_MIN"){
      bit=1u<<7; if(!parseUIntValue(value,1,1440,n)) ok=false; else c.logIntervalMin=(uint16_t)n;
    }else if(key=="PHOTO_INTERVAL_MIN"){
      bit=1u<<8; if(!parseUIntValue(value,1,1440,n)) ok=false; else c.photoIntervalMin=(uint16_t)n;
    }else ok=false;
    if(bit){ if(seen&bit) ok=false; else seen|=bit; }}
  f.close(); unmountSD(); const uint16_t required=(1u<<9)-1u;
  if(!ok||seen!=required||!validateConfig(c)) return false;
  loaded=c; return true;}

bool writeConfigToSD(const AppConfig &c){
  if(!validateConfig(c)||!mountSD()) return false;
  SD.remove("/config.tmp"); File f=SD.open("/config.tmp",FILE_WRITE);
  if(!f){ unmountSD(); return false; }
  f.print("CONFIG_VERSION=");f.println(CONFIG_VERSION); f.print("DEVICE_NAME=");f.println(c.deviceName); f.print("WIFI_SSID=");f.println(c.wifiSsid);
  f.print("WIFI_PASSWORD=");f.println(c.wifiPassword); f.print("MQTT_SERVER=");f.println(c.mqttServer); f.print("MQTT_PORT=");f.println(c.mqttPort);
  f.print("TRIAL_NAME=");f.println(c.trialName); f.print("LOG_INTERVAL_MIN=");f.println(c.logIntervalMin); f.print("PHOTO_INTERVAL_MIN=");f.println(c.photoIntervalMin);
  f.flush(); bool ok=(bool)f; f.close();
  if(ok){ SD.remove("/config.txt"); ok=SD.rename("/config.tmp","/config.txt"); }
  if(!ok) SD.remove("/config.tmp");
  unmountSD(); sdReady=ok; return ok;}

DateTime nowRTC(){ return rtcReady?rtc.now():DateTime(2000,1,1,0,0,0); }
bool saveMeasurement(const float v[],const char* source){
  if(!mountSD()) return false;
  bool newFile=!SD.exists("/DATA.CSV"); File f=SD.open("/DATA.CSV",FILE_APPEND);
  if(!f){ unmountSD(); sdReady=false; return false; }
  if(newFile) f.println("date,time,trial,temp_C,humidity_pct,pressure_hPa,lux,mq135_raw,mq135_base,mq135_increase,source");
  DateTime n=nowRTC(); char stamp[32]; snprintf(stamp,sizeof(stamp),"%04d-%02d-%02d,%02d:%02d:%02d",n.year(),n.month(),n.day(),n.hour(),n.minute(),n.second());
  f.print(stamp); f.print(','); f.print(cfg.trialName);
  for(uint8_t i=0;i<NM;i++){
    f.print(',');
    if(isfinite(v[i])) f.print(v[i],i==4?0:(i==3?1:2));}
  f.print(',');
  if(gasCalibrated && isfinite(gasBaseline)) f.print(gasBaseline,0);
  f.print(',');
  if(gasCalibrated && isfinite(v[4]) && gasBaseline>0) f.print(v[4]/gasBaseline,3);
  f.print(','); f.println(source); f.close(); unmountSD(); sdReady=true;
  return true;}

bool saveJPEG(const char* name,uint8_t* buf,size_t len){
  if(!mountSD()) return false;
  SD.remove(name); File f=SD.open(name,FILE_WRITE);
  if(!f){ unmountSD(); sdReady=false; return false; }
  size_t n=f.write(buf,len); f.close(); unmountSD(); sdReady=(n==len);
  return sdReady;}

bool initCamera(){
  camera_config_t c={}; c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0; c.pin_d0=CAM_Y2; c.pin_d1=CAM_Y3; c.pin_d2=CAM_Y4; c.pin_d3=CAM_Y5;
  c.pin_d4=CAM_Y6; c.pin_d5=CAM_Y7; c.pin_d6=CAM_Y8; c.pin_d7=CAM_Y9; c.pin_xclk=CAM_XCLK; c.pin_pclk=CAM_PCLK; c.pin_vsync=CAM_VSYNC; c.pin_href=CAM_HREF;
  c.pin_sccb_sda=-1; c.pin_sccb_scl=-1; c.sccb_i2c_port=0; c.pin_pwdn=CAM_PWDN; c.pin_reset=CAM_RESET; c.xclk_freq_hz=10000000;
  c.pixel_format=PIXFORMAT_RGB565; c.frame_size=FRAMESIZE_QVGA; c.jpeg_quality=10; c.fb_count=1; c.fb_location=psramFound()?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM;
  return esp_camera_init(&c)==ESP_OK;}

enum PhotoCapture : uint8_t { PHOTO_OK, PHOTO_FRAME_FAIL, PHOTO_JPEG_FAIL };

uint8_t captureJPEG(uint8_t*& jpg,size_t& len){
  jpg=nullptr; len=0; if(mcpReady)mcp.digitalWrite(MCP_PHOTO_LED,HIGH); delay(300);
  camera_fb_t* discard=esp_camera_fb_get(); if(discard)esp_camera_fb_return(discard); camera_fb_t* frame=esp_camera_fb_get();
  if(!frame){ if(mcpReady)mcp.digitalWrite(MCP_PHOTO_LED,LOW); return PHOTO_FRAME_FAIL; }
  bool ok=frame2jpg(frame,80,&jpg,&len); esp_camera_fb_return(frame); if(mcpReady)mcp.digitalWrite(MCP_PHOTO_LED,LOW);
  if(!ok||!jpg||!len){ if(jpg){free(jpg);jpg=nullptr;} len=0; return PHOTO_JPEG_FAIL; } return PHOTO_OK;}
void photoFilename(char* out,size_t n){
  if(rtcReady){ DateTime t=rtc.now(); snprintf(out,n,"/IMG_%04d%02d%02d_%02d%02d%02d.jpg",t.year(),t.month(),t.day(),t.hour(),t.minute(),t.second()); }
  else snprintf(out,n,"/IMG_%lu.jpg",millis());}

void messageScreen(const char* title,const char* a,const char* b=nullptr,const char* c=nullptr){
  tft.fillScreen(C_BG); tft.setTextWrap(true); tft.setTextSize(2); tft.setTextColor(ST77XX_CYAN); tft.setCursor(4,4); tft.println(title);
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(4,34);
  if(a){tft.println(a);tft.println();} if(b){tft.println(b);tft.println();} if(c)tft.println(c);}

void bootHeader(){
  tft.fillScreen(C_BG); tft.setTextWrap(false); tft.setTextSize(2); tft.setTextColor(ST77XX_CYAN); tft.setCursor(4,3); tft.print("FOOD MONITOR");
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(4,21); tft.print("Hardware self-test");}

void bootStatus(uint8_t row,const char* name,bool good){
  int y=34+row*10; tft.fillRect(0,y,160,10,C_BG); tft.setCursor(4,y); tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.print(name);
  tft.setCursor(128,y); tft.setTextColor(good?ST77XX_GREEN:ST77XX_RED); tft.print(good?"OK":"FAIL");}

void bootWorking(uint8_t row,const char* name){
  int y=34+row*10; tft.fillRect(0,y,160,10,C_BG); tft.setCursor(4,y); tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.print(name);
  tft.setCursor(112,y); tft.setTextColor(ST77XX_YELLOW); tft.print("TEST...");}

void plot(uint8_t m,int16_t x,int16_t y,int16_t w,int16_t h,float lo,float hi){
  if(count==0||hi<=lo) return;
  bool havePrev=false; int16_t px=0,py=0;
  for(uint8_t k=0;k<count;k++){
    float v=histAt(m,k); if(!isfinite(v)){havePrev=false;continue;}
    int16_t cx=x+(w-1)-(long)(count-1-k)*(w-1)/(HIST-1);
    float norm=(v-lo)/(hi-lo); if(norm<0)norm=0; if(norm>1)norm=1;
    int16_t cy=y+(h-1)-(int16_t)lroundf(norm*(h-1));
    if(havePrev)tft.drawLine(px,py,cx,cy,M[m].color); else tft.drawPixel(cx,cy,M[m].color);
    px=cx;py=cy;havePrev=true;}}

void dottedHLine(int16_t x,int16_t y,int16_t w,uint16_t color){
  for(int16_t xx=x;xx<x+w;xx+=4){
    int16_t n=min((int16_t)2,(int16_t)(x+w-xx)); tft.drawFastHLine(xx,y,n,color);}}

int16_t gasRatioY(float ratio,int16_t y,int16_t h,float lo,float hi){
  float norm=(ratio-lo)/(hi-lo);
  if(norm<0)norm=0; if(norm>1)norm=1;
  return y+(h-1)-(int16_t)lroundf(norm*(h-1));}

void drawWifiIcon(int16_t x,int16_t y,bool online){
  uint16_t c=online?ST77XX_GREEN:ST77XX_RED; tft.drawPixel(x+5,y+7,c); tft.drawLine(x+3,y+5,x+5,y+3,c); tft.drawLine(x+5,y+3,x+7,y+5,c);
  tft.drawLine(x+1,y+3,x+3,y+1,c); tft.drawFastHLine(x+3,y+1,5,c); tft.drawLine(x+8,y+1,x+10,y+3,c);}

const char* gasRiskText(){
  if(!gasCalibrated || !isfinite(currentGasRatio)) return "N/A.CAL";
  if(currentGasRatio>=GAS_DANGER_RATIO) return "DANGER";
  if(currentGasRatio>=GAS_CAUTION_RATIO) return "CAUTION";
  return "SAFE";}

uint16_t gasRiskColor(){
  if(!gasCalibrated || !isfinite(currentGasRatio)) return ST77XX_YELLOW;
  if(currentGasRatio>=GAS_DANGER_RATIO) return ST77XX_RED;
  if(currentGasRatio>=GAS_CAUTION_RATIO) return ST77XX_YELLOW;
  return ST77XX_GREEN;}

void drawStatusHeader(){
  tft.fillRect(0,0,160,12,C_HDR); tft.setTextSize(1); unsigned long elapsed=millis()-lastLog; unsigned long interval=logIntervalMs();
  unsigned long remain=(elapsed>=interval)?0:(interval-elapsed); unsigned long sec=(remain+999UL)/1000UL, mins=sec/60UL; sec%=60UL;
  tft.setTextColor(C_DIM); tft.setCursor(2,2); tft.print("T-");
  if(mins<10)tft.print('0'); tft.print(mins); tft.print(':'); if(sec<10)tft.print('0'); tft.print(sec);
  tft.fillCircle(49,5,3,sdReady?ST77XX_GREEN:ST77XX_RED); tft.setTextColor(ST77XX_WHITE); tft.setCursor(54,2); tft.print("SD"); drawWifiIcon(70,1,mqttReady);
  tft.setTextColor(gasRiskColor()); tft.setCursor(84,2); tft.print(gasRiskText());
  tft.setTextColor(C_DIM); tft.setCursor(140,2); tft.print(page+1); tft.print('/'); tft.print(NPAGES);}

void drawOverview(){
  const int16_t top=13,rowH=23;
  for(uint8_t m=0;m<NM;m++){
    int16_t ry=top+m*rowH; float lo,hi; tft.fillRect(0,ry,160,rowH-1,C_BG); tft.setTextSize(1); tft.setTextColor(M[m].color); tft.setCursor(2,ry+2); tft.print(M[m].shortName);
    if(m==4 && !gasCalibrated){
      tft.setTextColor(ST77XX_YELLOW); tft.setCursor(22,ry+2); tft.print("calibrating");}
    tft.setTextColor(ST77XX_WHITE); tft.setCursor(2,ry+12); printValue(currentValue[m],M[m].dec); tft.print(M[m].unit); plotRange(m,lo,hi);
    if(!(m==4 && !gasCalibrated)){
      plot(m,62,ry+2,96,rowH-5,lo,hi);
      if(m==4 && gasCalibrated){
        if(GAS_CAUTION_RATIO>=lo && GAS_CAUTION_RATIO<=hi) dottedHLine(62,gasRatioY(GAS_CAUTION_RATIO,ry+2,rowH-5,lo,hi),96,ST77XX_YELLOW);
        if(GAS_DANGER_RATIO>=lo && GAS_DANGER_RATIO<=hi) dottedHLine(62,gasRatioY(GAS_DANGER_RATIO,ry+2,rowH-5,lo,hi),96,ST77XX_RED);}}
    tft.drawFastHLine(0,ry+rowH-1,160,C_GRID);}}

void drawMetric(uint8_t m){
  float rlo=NAN,rhi=NAN,lo,hi; bool have=minMax(m,rlo,rhi); plotRange(m,lo,hi); tft.fillRect(0,13,160,115,C_BG); tft.setTextSize(1);
  if(m==4){
    tft.setTextColor(C_DIM); tft.setCursor(3,15);
    if(gasCalibrated){tft.print("Base: ");tft.print(gasBaseline,0);tft.print(" raw");}
    else{
      unsigned long warmElapsed=millis()-gasWarmupStarted;
      if(warmElapsed<GAS_WARMUP_MS){
        unsigned long left=(GAS_WARMUP_MS-warmElapsed+999UL)/1000UL;
        tft.setTextColor(ST77XX_YELLOW);tft.print("Calibrating - warmup ");tft.print(left/60UL);tft.print(':');if((left%60UL)<10)tft.print('0');tft.print(left%60UL);}
        else{tft.setTextColor(ST77XX_YELLOW);tft.print("Calibrating - stable ");tft.print(gasBaselineCount);tft.print('/');tft.print(GAS_BASELINE_SAMPLES);}}
    tft.setTextSize(2);tft.setTextColor(ST77XX_WHITE);tft.setCursor(3,26);printValue(currentValue[m],0);
    tft.setTextSize(1);tft.setTextColor(M[m].color);tft.setCursor(tft.getCursorX()+4,33);tft.print("raw"); tft.setCursor(3,43);
    if(gasCalibrated){tft.setTextColor(ST77XX_MAGENTA);tft.print("Increase: ");tft.print(currentGasRatio,2);tft.print('x');}
    else{tft.setTextColor(C_DIM);tft.print("Increase: --");}
    const int16_t gx=40,gy=54,gw=118,gh=62; tft.drawFastVLine(gx-1,gy,gh+1,C_GRID);tft.drawFastHLine(gx-1,gy+gh,gw+1,C_GRID);tft.drawFastHLine(gx,gy+gh/2,gw,C_GRID);
    tft.setTextColor(C_DIM); tft.setCursor(0,gy);tft.print(hi,0);tft.print('x'); tft.setCursor(0,gy+gh/2-3);tft.print((hi+lo)/2.0f,1);tft.print('x');
    tft.setCursor(0,gy+gh-7);tft.print(lo,1);tft.print('x');
    if(gasCalibrated){
      if(GAS_CAUTION_RATIO>=lo && GAS_CAUTION_RATIO<=hi){
        int16_t cy=gasRatioY(GAS_CAUTION_RATIO,gy,gh,lo,hi); dottedHLine(gx,cy,gw,ST77XX_YELLOW);
        tft.setTextColor(ST77XX_YELLOW);tft.setCursor(8,cy-3);tft.print(GAS_CAUTION_RATIO,1);tft.print('x');}
      if(GAS_DANGER_RATIO>=lo && GAS_DANGER_RATIO<=hi){
        int16_t dy=gasRatioY(GAS_DANGER_RATIO,gy,gh,lo,hi); dottedHLine(gx,dy,gw,ST77XX_RED); tft.setTextColor(ST77XX_RED);tft.setCursor(12,dy-3);tft.print("5x");}}
    float spanSec=(count>0?(count-1):0)*(GRAPH_INTERVAL_MS/1000.0f); tft.setTextColor(C_DIM);tft.setCursor(gx,120);tft.print('-');
    if(spanSec<60.0f){tft.print(spanSec,0);tft.print('s');}
    else if(spanSec<3600.0f){tft.print(spanSec/60.0f,1);tft.print('m');}
    else{tft.print(spanSec/3600.0f,1);tft.print('h');}
    tft.setCursor(gx+gw-18,120);tft.print("now");
    if(gasCalibrated) plot(m,gx,gy,gw,gh,lo,hi);
    return;}

  tft.setTextColor(M[m].color);tft.setCursor(3,15);tft.print(M[m].shortName);
  tft.setTextSize(2);tft.setTextColor(ST77XX_WHITE);tft.setCursor(3,24);printValue(currentValue[m],M[m].dec);
  tft.setTextSize(1);tft.setTextColor(M[m].color);tft.setCursor(tft.getCursorX()+4,31);tft.print(M[m].unit);
  tft.setTextColor(C_DIM);tft.setCursor(3,40);tft.print("min ");if(have)printValue(rlo,M[m].dec);else tft.print("--");
  tft.print("  max ");if(have)printValue(rhi,M[m].dec);else tft.print("--");
  const int16_t gx=40,gy=50,gw=118,gh=66; tft.drawFastVLine(gx-1,gy,gh+1,C_GRID);tft.drawFastHLine(gx-1,gy+gh,gw+1,C_GRID);tft.drawFastHLine(gx,gy+gh/2,gw,C_GRID);
  tft.setTextColor(C_DIM); uint8_t ld=(hi-lo<10.0f)?min(M[m].dec,(uint8_t)1):0;
  tft.setCursor(0,gy);tft.print(hi,ld);tft.setCursor(0,gy+gh/2-3);tft.print((hi+lo)/2.0f,ld);tft.setCursor(0,gy+gh-7);tft.print(lo,ld);
  float spanSec=(count>0?(count-1):0)*(GRAPH_INTERVAL_MS/1000.0f); tft.setTextColor(C_DIM);tft.setCursor(gx,120);tft.print('-');
  if(spanSec<60.0f){tft.print(spanSec,0);tft.print('s');}
  else if(spanSec<3600.0f){tft.print(spanSec/60.0f,1);tft.print('m');}
  else{tft.print(spanSec/3600.0f,1);tft.print('h');}
  tft.setCursor(gx+gw-18,120);tft.print("now"); plot(m,gx,gy,gw,gh,lo,hi);}

void drawPage(bool full){
  if(full)tft.fillScreen(C_BG);
  drawStatusHeader();
  if(page==0) drawOverview();
  else drawMetric(page-1);}

void drawConfigErrorScreen(){
  tft.fillScreen(C_BG); tft.setTextWrap(false); tft.setTextSize(2); tft.setTextColor(ST77XX_RED); tft.setCursor(3,5); tft.print("CONFIG ERROR");
  tft.drawFastHLine(3,25,154,C_GRID); tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(3,35); tft.print("config.txt error");
  tft.setTextColor(C_DIM); tft.setCursor(3,52); tft.print("Continue with default"); tft.setCursor(3,63); tft.print("values or enable serial.");
  tft.drawFastHLine(3,91,154,C_GRID); tft.drawRect(2,105,62,18,C_GRID); tft.setTextColor(ST77XX_GREEN); tft.setCursor(6,111); tft.print("[DEFAULT]");
  tft.drawRect(103,105,55,18,C_GRID); tft.setTextColor(ST77XX_CYAN); tft.setCursor(108,111); tft.print("[SERIAL]");}

enum ConfigChoice : uint8_t { CONFIG_DEFAULT, CONFIG_SERIAL };

uint8_t waitForConfigChoice(){
  drawConfigErrorScreen();
  if(!mcpReady){ delay(1800); return CONFIG_DEFAULT; }
  bool prevA=mcp.digitalRead(MCP_ACTION), prevP=mcp.digitalRead(MCP_PAGE);
  while(true){
    bool a=mcp.digitalRead(MCP_ACTION), p=mcp.digitalRead(MCP_PAGE);
    if(prevA==HIGH&&a==LOW){ delay(DEBOUNCE_MS); while(mcp.digitalRead(MCP_ACTION)==LOW) delay(5); return CONFIG_DEFAULT; }
    if(prevP==HIGH&&p==LOW){ delay(DEBOUNCE_MS); while(mcp.digitalRead(MCP_PAGE)==LOW) delay(5); return CONFIG_SERIAL; }
    prevA=a; prevP=p; delay(10);}}

void restoreTFTAfterSerial(){
  Serial.flush(); Serial.end(); delay(50); pinMode(TFT_CS,OUTPUT); pinMode(TFT_DC,OUTPUT); digitalWrite(TFT_CS,HIGH); SPI.begin(SPI_SCK,SPI_MISO,SPI_MOSI,-1);
  tft.initR(INITR_BLACKTAB); tft.setRotation(ROTATION); tft.setTextWrap(false);}

void serialSetupError(){
  restoreTFTAfterSerial(); tft.fillScreen(C_BG); tft.setTextSize(2); tft.setTextColor(ST77XX_RED); tft.setCursor(3,12); tft.print("SERIAL ERROR");
  tft.drawFastHLine(3,34,154,C_GRID); tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(3,52); tft.print("[SERIAL ERROR,");
  tft.setCursor(3,65); tft.print("RESET AND TRY AGAIN]");
  while(true) delay(1000);}

bool readSerialLine(String &outLine,uint32_t timeoutMs){
  outLine=""; unsigned long started=millis();
  while(millis()-started<timeoutMs){
    while(Serial.available()){
      char c=(char)Serial.read();
      if(c=='\r') continue;
      if(c=='\n') return true;
      if((uint8_t)c>=32 && outLine.length()<127) outLine+=c;}
    delay(5);} return false;}

bool serialPrompt(const char* prompt,String &value,bool allowEmpty,uint16_t maxLen){
  while(true){
    Serial.println(prompt);
    if(!readSerialLine(value,120000UL)) return false;
    if(value.length()<=maxLen && (allowEmpty||value.length()>0)) return true;
    Serial.println("Invalid value, try again.");}}

bool serialPromptUInt(const char* prompt,uint16_t &value,uint16_t minV,uint16_t maxV,bool blankDefault,uint16_t defaultV){
  while(true){
    Serial.println(prompt); String s;
    if(!readSerialLine(s,120000UL)) return false;
    if(blankDefault && !s.length()){ value=defaultV; return true; }
    uint32_t n=0;
    if(parseUIntValue(s,minV,maxV,n)){ value=(uint16_t)n; return true; }
    Serial.println("Invalid number, try again.");}}

void runSerialConfig(){
  // GPIO1 is TFT DC normally and UART TX during serial setup, so the LCD freezes here.
  tft.fillScreen(C_BG); tft.setTextSize(2); tft.setTextColor(ST77XX_CYAN); tft.setCursor(3,6); tft.print("SERIAL"); tft.drawFastHLine(3,28,154,C_GRID);
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(3,40); tft.print("Serial Enabled -"); tft.setCursor(3,52); tft.print("LCD Frozen.");
  tft.setTextColor(C_DIM); tft.setCursor(3,72); tft.print("Continue in Serial Monitor"); tft.setCursor(3,84); tft.print("115200 baud / Newline"); deselectTFT();
  Serial.begin(115200); delay(100); unsigned long started=millis(), lastPrompt=0; bool connected=false;
  while(millis()-started<60000UL){
    if(millis()-lastPrompt>=2000UL || lastPrompt==0){ Serial.println("Food Monitor setup: press ENTER to begin."); lastPrompt=millis(); }
    if(Serial.available()){
      String handshake;
      if(readSerialLine(handshake,3000UL)){ connected=true; break; }}
    delay(10);}
  if(!connected) serialSetupError();
  AppConfig c={}; String s; Serial.println(); Serial.println("--- Food Monitor config ---");
  if(!serialPrompt("Device name:",s,false,sizeof(c.deviceName)-1) || !copyConfigValue(c.deviceName,sizeof(c.deviceName),s)) serialSetupError();
  if(!serialPrompt("Wi-Fi SSID:",s,true,sizeof(c.wifiSsid)-1) || !copyConfigValue(c.wifiSsid,sizeof(c.wifiSsid),s)) serialSetupError();
  if(!serialPrompt("Wi-Fi password:",s,true,sizeof(c.wifiPassword)-1) || !copyConfigValue(c.wifiPassword,sizeof(c.wifiPassword),s)) serialSetupError();
  if(!serialPrompt("MQTT server:",s,true,sizeof(c.mqttServer)-1) || !copyConfigValue(c.mqttServer,sizeof(c.mqttServer),s)) serialSetupError();
  if(!serialPromptUInt("MQTT port:",c.mqttPort,1,65535,false,DEFAULT_MQTT_PORT)) serialSetupError();
  if(!serialPrompt("Trial name:",s,false,sizeof(c.trialName)-1) || !copyConfigValue(c.trialName,sizeof(c.trialName),s)) serialSetupError();
  if(!serialPromptUInt("Log interval (minutes) [Enter Blank for 30 mins]:",c.logIntervalMin,1,1440,true,30)) serialSetupError();
  if(!serialPromptUInt("Photo interval (minutes) [Enter Blank for 30 mins]:",c.photoIntervalMin,1,1440,true,30)) serialSetupError();
  if(!validateConfig(c)){ Serial.println("Config validation failed."); serialSetupError(); }
  if(!writeConfigToSD(c)){ Serial.println("Could not write /config.txt."); serialSetupError(); }
  AppConfig verify={};
  if(!loadConfigFromSD(verify)){ Serial.println("Config read-back failed."); serialSetupError(); }
  Serial.println("Config complete. Resetting..."); Serial.flush(); restoreTFTAfterSerial();
  tft.fillScreen(C_BG); tft.setTextSize(2); tft.setTextColor(ST77XX_GREEN); tft.setCursor(3,20); tft.print("CONFIG COMPLETE"); tft.drawFastHLine(3,43,154,C_GRID);
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(3,58); tft.print("Config complete,"); tft.setCursor(3,70); tft.print("resetting...");
  delay(1200); ESP.restart(); while(true) delay(1000);}

bool wipeDirectoryContents(const char* dirname){
  while(true){
    File dir=SD.open(dirname,FILE_READ);
    if(!dir || !dir.isDirectory()){ if(dir)dir.close(); return false; }
    File entry=dir.openNextFile();
    if(!entry){ dir.close(); return true; }
    String path=entry.path(); bool isDir=entry.isDirectory(); entry.close(); dir.close();
    if(!path.length() || path=="/") return false;
    if(isDir){
      if(!wipeDirectoryContents(path.c_str())) return false;
      if(!SD.rmdir(path.c_str())) return false;
    }else if(!SD.remove(path.c_str())) return false;
    delay(1);}}

bool wipeSDCard(){
  if(!mountSD()) return false;
  bool ok=wipeDirectoryContents("/"); unmountSD(); sdReady=false;
  return ok;}

void drawFullResetCountdown(uint8_t secondsLeft){
  tft.fillScreen(C_BG); tft.setTextWrap(false); tft.setTextSize(2); tft.setTextColor(ST77XX_RED); tft.setCursor(18,5); tft.print("FULL RESET");
  tft.drawFastHLine(3,27,154,C_GRID); tft.setTextSize(1); tft.setTextColor(ST77XX_RED); tft.setCursor(11,39); tft.print("ALL DATA WILL BE WIPED");
  tft.setTextColor(C_DIM); tft.setCursor(29,52); tft.print("Keep both held"); tft.setTextSize(4); tft.setTextColor(ST77XX_YELLOW); tft.setCursor(68,66); tft.print(secondsLeft);
  tft.setTextSize(1); tft.setTextColor(C_DIM); tft.setCursor(26,113); tft.print("Release to cancel");}

bool confirmFullReset(){
  for(int8_t sec=5;sec>=1;sec--){
    drawFullResetCountdown((uint8_t)sec); unsigned long tick=millis();
    while(millis()-tick<1000UL){
      if(!mcpReady || mcp.digitalRead(MCP_ACTION)!=LOW || mcp.digitalRead(MCP_PAGE)!=LOW){
        drawPage(true);
        return false;}
      delay(10);}}
  tft.fillScreen(C_BG); tft.setTextSize(2); tft.setTextColor(ST77XX_RED); tft.setCursor(20,12); tft.print("WIPING SD"); tft.drawFastHLine(3,35,154,C_GRID);
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(24,53); tft.print("Deleting all data..."); bool ok=wipeSDCard();
  if(!ok){
    tft.fillScreen(C_BG); tft.setTextSize(2); tft.setTextColor(ST77XX_RED); tft.setCursor(8,15); tft.print("RESET FAILED"); tft.drawFastHLine(3,39,154,C_GRID);
    tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(24,56); tft.print("SD wipe failed.");
    tft.setTextColor(C_DIM); tft.setCursor(12,72); tft.print("No restart performed."); delay(1800); drawPage(true);
    return false;}
  tft.fillScreen(C_BG); tft.setTextSize(2); tft.setTextColor(ST77XX_GREEN); tft.setCursor(4,15); tft.print("RESET COMPLETE"); tft.drawFastHLine(3,39,154,C_GRID);
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(22,56); tft.print("All SD data wiped.");
  tft.setTextColor(C_DIM); tft.setCursor(40,72); tft.print("Resetting..."); delay(1200); ESP.restart();
  while(true) delay(1000);}

void drawActionProgress(const char* label,const char* detail){
  tft.fillScreen(C_BG); drawStatusHeader(); tft.setTextSize(1); tft.setTextColor(C_DIM); tft.setCursor(3,17); tft.print(label); tft.drawFastHLine(3,28,154,C_GRID);
  tft.setTextSize(2); tft.setTextColor(ST77XX_CYAN); tft.setCursor(3,39); tft.print("WORKING");
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(3,66); tft.print(detail);}

void drawStorageRow(int16_t y,bool good,const char* label,const char* state){
  tft.fillCircle(7,y+4,3,good?ST77XX_GREEN:ST77XX_RED); tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(14,y); tft.print(label);
  tft.setTextColor(good?ST77XX_GREEN:ST77XX_RED); tft.setCursor(112,y); tft.print(state);}

void drawNetworkRow(int16_t y,bool sent,bool attempted){
  drawWifiIcon(2,y-1,sent); tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(16,y); tft.print("Network"); tft.setCursor(100,y);
  if(sent){ tft.setTextColor(ST77XX_GREEN); tft.print("SENT"); }
  else if(!attempted){ tft.setTextColor(C_DIM); tft.print("SKIPPED"); }
  else if(!mqttClient.connected()){ tft.setTextColor(ST77XX_RED); tft.print("OFFLINE"); }
  else{ tft.setTextColor(ST77XX_RED); tft.print("FAILED"); }}

void drawMeasurementResult(const float v[],bool sdOk,bool mqttOk,bool mqttAttempted){
  tft.fillScreen(C_BG); drawStatusHeader(); tft.setTextSize(1); tft.setTextColor(C_DIM); tft.setCursor(3,16); tft.print("MANUAL SAMPLE");
  tft.setTextSize(2); tft.setTextColor(sdOk?ST77XX_GREEN:ST77XX_RED); tft.setCursor(3,27); tft.print(sdOk?"SAVED":"SAVE FAIL"); tft.drawFastHLine(3,47,154,C_GRID);
  drawStorageRow(52,sdOk,"SD card",sdOk?"OK":"FAIL"); drawNetworkRow(65,mqttOk,mqttAttempted); tft.drawFastHLine(3,78,154,C_GRID);
  tft.setTextSize(1); tft.setTextColor(C_DIM); tft.setCursor(3,84); tft.print("Temp");
  tft.setTextColor(ST77XX_WHITE); tft.setCursor(42,84); printValue(v[0],1); tft.print(" C"); tft.setTextColor(C_DIM); tft.setCursor(3,96); tft.print("Gas");
  tft.setTextColor(ST77XX_WHITE); tft.setCursor(42,96); printValue(v[4],0); tft.print(" raw"); tft.setTextColor(C_DIM); tft.setCursor(3,108); tft.print("Increase");
  tft.setCursor(58,108);
  if(gasCalibrated && isfinite(v[4]) && gasBaseline>0){ tft.setTextColor(gasRiskColor()); tft.print(v[4]/gasBaseline,2); tft.print('x'); }
  else{ tft.setTextColor(C_DIM); tft.print("--"); }}

void drawPhotoResult(const char* filename,size_t bytes,bool sdOk,bool mqttOk,bool mqttAttempted){
  tft.fillScreen(C_BG); drawStatusHeader(); tft.setTextSize(1); tft.setTextColor(C_DIM); tft.setCursor(3,16); tft.print("PHOTO");
  tft.setTextSize(2); tft.setTextColor(sdOk?ST77XX_GREEN:ST77XX_RED); tft.setCursor(3,27); tft.print(sdOk?"SAVED":"SAVE FAIL"); tft.drawFastHLine(3,47,154,C_GRID);
  drawStorageRow(52,sdOk,"SD card",sdOk?"OK":"FAIL"); drawNetworkRow(65,mqttOk,mqttAttempted); tft.drawFastHLine(3,78,154,C_GRID);
  tft.setTextSize(1); tft.setTextColor(C_DIM); tft.setCursor(3,85); tft.print("File"); tft.setTextColor(ST77XX_WHITE); tft.setCursor(30,85);
  const char* shortName=filename; if(shortName[0]=='/') shortName++;
  if(strlen(shortName)>20) shortName+=strlen(shortName)-20;
  tft.print(shortName); tft.setTextColor(C_DIM); tft.setCursor(3,99); tft.print("Size");
  tft.setTextColor(ST77XX_WHITE); tft.setCursor(30,99); tft.print((unsigned int)bytes); tft.print(" B");}

void manualMeasurement(){
  float v[NM]; readSensors(v); drawActionProgress("MANUAL SAMPLE","Writing DATA.CSV..."); bool ok=saveMeasurement(v,"MANUAL");
  bool mqttAttempted=ok && mqttClient.connected(); bool mqttOk=ok && publishTelemetry(v,"MANUAL"); drawMeasurementResult(v,ok,mqttOk,mqttAttempted); delay(1800); drawPage(true);}

void takePhoto(){
  if(!cameraReady){messageScreen("CAMERA","Camera unavailable.");delay(1200);drawPage(true);return;}
  drawActionProgress("PHOTO","Capturing image..."); uint8_t* jpg=nullptr; size_t len=0; uint8_t cap=captureJPEG(jpg,len);
  if(cap!=PHOTO_OK){messageScreen("PHOTO FAIL",cap==PHOTO_FRAME_FAIL?"No camera frame.":"JPEG conversion failed.");delay(1200);drawPage(true);return;}
  char filename[48]; photoFilename(filename,sizeof(filename)); drawActionProgress("PHOTO","Writing JPEG...");
  bool ok=saveJPEG(filename,jpg,len), mqttAttempted=ok&&mqttClient.connected(), mqttOk=false;
  if(mqttAttempted){ publishPhotoEvent(filename,len); mqttOk=mqttClient.connected(); }
  free(jpg); drawPhotoResult(filename,len,ok,mqttOk,mqttAttempted); delay(1800); drawPage(true);}

void scheduledMeasurement(){float v[NM]; readSensors(v); bool ok=saveMeasurement(v,"SCHEDULED"); if(ok) publishTelemetry(v,"SCHEDULED");}

void scheduledPhoto(){
  if(!cameraReady) return; uint8_t* jpg=nullptr; size_t len=0; if(captureJPEG(jpg,len)!=PHOTO_OK) return;
  char filename[48]; photoFilename(filename,sizeof(filename)); bool ok=saveJPEG(filename,jpg,len); if(ok) publishPhotoEvent(filename,len); free(jpg);}

void setup(){
  applyDefaultConfig(cfg); pinMode(TFT_CS,OUTPUT); pinMode(SD_CS,OUTPUT); digitalWrite(TFT_CS,HIGH); digitalWrite(SD_CS,HIGH); SPI.begin(SPI_SCK,SPI_MISO,SPI_MOSI,-1);
  tft.initR(INITR_BLACKTAB); tft.setRotation(ROTATION); tft.setTextWrap(false); bootHeader(); Wire.begin(I2C_SDA,I2C_SCL); Wire.setClock(100000);
  bootWorking(0,"MCP23017"); mcpReady=mcp.begin_I2C(MCP_ADDR,&Wire);
  if(mcpReady){mcp.pinMode(MCP_ACTION,INPUT_PULLUP);mcp.pinMode(MCP_PAGE,INPUT_PULLUP);mcp.pinMode(MCP_PHOTO_LED,OUTPUT);mcp.digitalWrite(MCP_PHOTO_LED,LOW);}
  bootStatus(0,"MCP23017",mcpReady); bootWorking(1,"SD/config"); sdReady=testSD(); bootStatus(1,"SD/config",sdReady); AppConfig loaded={};
  if(sdReady && loadConfigFromSD(loaded)){ cfg=loaded; }
  else{
    uint8_t choice=waitForConfigChoice();
    if(choice==CONFIG_SERIAL) runSerialConfig();
    applyDefaultConfig(cfg);}
  bootHeader(); bootStatus(0,"MCP23017",mcpReady); bootWorking(1,"BME280");
  bmeReady=bme.begin(0x76,&Wire); if(!bmeReady)bmeReady=bme.begin(0x77,&Wire);
  bootStatus(1,"BME280",bmeReady); bootWorking(2,"BH1750"); bhReady=lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE,0x23,&Wire); bootStatus(2,"BH1750",bhReady);
  bootWorking(3,"DS3231"); rtcReady=rtc.begin(&Wire); bootStatus(3,"DS3231",rtcReady);
  bootWorking(4,"MQ135"); pinMode(MQ135_PIN,INPUT); analogReadResolution(12); analogSetPinAttenuation(MQ135_PIN,ADC_11db); gasWarmupStarted=millis(); bootStatus(4,"MQ135",true);
  bootStatus(5,"SD card",sdReady); bootWorking(6,"Camera"); cameraReady=initCamera(); bootStatus(6,"Camera",cameraReady);
  mqttClient.setServer(cfg.mqttServer,cfg.mqttPort); mqttClient.setCallback(mqttCallback); mqttClient.setBufferSize(768); mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(1); startWiFi(); float v[NM]; readSensors(v); pushHistory(v);
  if(sdReady){ bool ok=saveMeasurement(v,"STARTUP"); if(ok) publishTelemetry(v,"STARTUP");}
  delay(1200); lastLog=lastLive=lastGraph=lastSDCheck=lastPhoto=millis(); drawPage(true);}

void loop(){
  unsigned long now=millis(); bool action=mcpReady?mcp.digitalRead(MCP_ACTION):HIGH; bool pageBtn=mcpReady?mcp.digitalRead(MCP_PAGE):HIGH;
  if(ignoreButtonsUntilReleased){
    if(action==HIGH && pageBtn==HIGH){ignoreButtonsUntilReleased=false; prevAction=HIGH; prevPage=HIGH;}}
    else if(action==LOW && pageBtn==LOW){
    if(!bothGestureActive){ bothGestureActive=true; bothDownAt=now;}
    if(now-bothDownAt>=BOTH_HOLD_MS){
      confirmFullReset(); bothGestureActive=false; bothDownAt=0; ignoreButtonsUntilReleased=true; action=mcpReady?mcp.digitalRead(MCP_ACTION):HIGH;
      pageBtn=mcpReady?mcp.digitalRead(MCP_PAGE):HIGH; prevAction=action; prevPage=pageBtn; now=millis();}
  }else if(bothGestureActive){bothGestureActive=false; bothDownAt=0; ignoreButtonsUntilReleased=true;}
  else{
    if(prevPage==HIGH&&pageBtn==LOW)pageDownAt=now;
    if(prevPage==LOW&&pageBtn==HIGH&&now-pageDownAt>=DEBOUNCE_MS){page=(page+1)%NPAGES; drawPage(true);}
    if(prevAction==HIGH&&action==LOW)actionDownAt=now;
    if(prevAction==LOW&&action==HIGH&&now-actionDownAt>=DEBOUNCE_MS){unsigned long held=now-actionDownAt; if(held>=LONG_PRESS_MS)takePhoto(); else manualMeasurement();}
    prevAction=action; prevPage=pageBtn;}
  serviceNetwork(action==HIGH && pageBtn==HIGH);
  if(action==HIGH && pageBtn==HIGH){
    if(mqttMeasureRequest){ mqttMeasureRequest=false; manualMeasurement(); now=millis(); }
    if(mqttPhotoRequest){ mqttPhotoRequest=false; takePhoto(); now=millis(); }}
  if(now-lastLive>=LIVE_REFRESH_MS){
    lastLive=now; float v[NM]; readSensors(v);
    if(now-lastGraph>=GRAPH_INTERVAL_MS){lastGraph=now;pushHistory(v);}
    drawPage(false);}
  unsigned long logMs=logIntervalMs();
  if(now-lastLog>=logMs){lastLog+=logMs; scheduledMeasurement(); drawPage(false); now=millis();}
  unsigned long photoMs=photoIntervalMs();
  if(now-lastPhoto>=photoMs){lastPhoto=now; scheduledPhoto(); now=millis(); drawPage(false);}
  unsigned long sdCheckInterval=sdReady?SD_CHECK_MS:SD_RECOVERY_CHECK_MS;
  if(action==HIGH && pageBtn==HIGH && now-lastSDCheck>=sdCheckInterval){
    lastSDCheck=now; bool wasReady=sdReady; checkSDPresent();
    if(sdReady!=wasReady) drawStatusHeader();}
  delay(10);}
