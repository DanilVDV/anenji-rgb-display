#!/usr/bin/env python3
import json, os, serial, threading, time, subprocess, shutil
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT=os.getenv("ANENJI_PORT","/dev/ttyS1")
BAUD=int(os.getenv("ANENJI_BAUD","9600"))
SLAVE=int(os.getenv("ANENJI_SLAVE","1"))
HTTP_PORT=int(os.getenv("ANENJI_HTTP_PORT","8088"))
POLL_SEC=float(os.getenv("ANENJI_POLL_SEC","2.0"))
ALLOW_WRITE=os.getenv("ANENJI_ALLOW_WRITE","0")=="1"
LOG_FILE=os.getenv("ANENJI_LOG","/root/anenji/changes.log")
HISTORY_LEN=int(os.getenv("ANENJI_HISTORY","900"))
BMS_ENABLE=os.getenv("ANENJI_BMS_ENABLE","1")=="1"
BMS_CMD=os.getenv("ANENJI_BMS_CMD","saihang-bms")
BMS_POLL_SEC=float(os.getenv("ANENJI_BMS_POLL_SEC","60"))
BMS_IO_TIMEOUT_MS=int(os.getenv("ANENJI_BMS_IO_TIMEOUT_MS","3500"))
BMS_TOPOLOGY=os.getenv("ANENJI_BMS_TOPOLOGY","individual").strip().lower()
BMS_FAIL_THRESHOLD=int(os.getenv("ANENJI_BMS_FAIL_THRESHOLD","3"))
BMS_STALE_AFTER_SEC=float(os.getenv("ANENJI_BMS_STALE_AFTER_SEC",str(max(180.0,BMS_POLL_SEC*3.0))))
BMS_WARN_AGE_SEC=float(os.getenv("ANENJI_BMS_WARN_AGE_SEC",str(max(300.0,(BMS_POLL_SEC*2.0 if BMS_POLL_SEC>0 else 3600.0)))))
BMS_OLD_AGE_SEC=float(os.getenv("ANENJI_BMS_OLD_AGE_SEC",str(max(900.0,(BMS_POLL_SEC*5.0 if BMS_POLL_SEC>0 else 14400.0)))))
BMS_MACS=[x.strip() for x in os.getenv("ANENJI_BMS_MACS","").split(",") if x.strip()]
BMS_EXPECTED_PACKS=int(os.getenv("ANENJI_BMS_EXPECTED_PACKS","4"))
BMS_PARALLEL=max(1,int(os.getenv("ANENJI_BMS_PARALLEL","2")))
BMS_RETRY_FAILED=max(0,int(os.getenv("ANENJI_BMS_RETRY_FAILED","1")))
BMS_RETRY_DELAY_SEC=float(os.getenv("ANENJI_BMS_RETRY_DELAY_SEC","0.8"))
BMS_AUTO_ON_DISCHARGE=os.getenv("ANENJI_BMS_AUTO_ON_DISCHARGE","1")=="1"
BMS_DISCHARGE_THRESHOLD_W=float(os.getenv("ANENJI_BMS_DISCHARGE_THRESHOLD_W","100"))
BMS_DISCHARGE_POLL_SEC=float(os.getenv("ANENJI_BMS_DISCHARGE_POLL_SEC","60"))
BMS_POST_RECOVERY_POLLS=max(0,int(os.getenv("ANENJI_BMS_POST_RECOVERY_POLLS","3")))
BMS_POST_RECOVERY_POLL_SEC=float(os.getenv("ANENJI_BMS_POST_RECOVERY_POLL_SEC","20"))
BATTERY_RESERVE_SOC=float(os.getenv("ANENJI_BATTERY_RESERVE_SOC","15"))
BATTERY_FULL_TARGET_SOC=float(os.getenv("ANENJI_BATTERY_FULL_TARGET_SOC","100"))
ETA_AVG_SEC=float(os.getenv("ANENJI_ETA_AVG_SEC","600"))
ETA_MIN_POWER_W=float(os.getenv("ANENJI_ETA_MIN_POWER_W","100"))
BMS_ACTIVE_CURRENT_A=float(os.getenv("ANENJI_BMS_ACTIVE_CURRENT_A","1.0"))
BMS_INACTIVE_CYCLES=max(1,int(os.getenv("ANENJI_BMS_INACTIVE_CYCLES","3")))
BMS_CRITICAL_TEMP_C=float(os.getenv("ANENJI_BMS_CRITICAL_TEMP_C","55"))
BMS_CELL_DELTA_WARN_MV=float(os.getenv("ANENJI_BMS_CELL_DELTA_WARN_MV","80"))
BMS_CELL_DELTA_ALARM_MV=float(os.getenv("ANENJI_BMS_CELL_DELTA_ALARM_MV","150"))
ATTENTION_ENABLE=os.getenv("ANENJI_ATTENTION_ENABLE","1")=="1"
ATTENTION_ON_DELAY_SEC=float(os.getenv("ANENJI_ATTENTION_ON_DELAY_SEC","120"))
ATTENTION_OFF_DELAY_SEC=float(os.getenv("ANENJI_ATTENTION_OFF_DELAY_SEC","60"))
ATTENTION_BATTERY_POWER_W=float(os.getenv("ANENJI_ATTENTION_BATTERY_POWER_W","150"))
ATTENTION_REQUIRE_GRID_ABSENT=os.getenv("ANENJI_ATTENTION_REQUIRE_GRID_ABSENT","1")=="1"
GRID_PRESENT_V=float(os.getenv("ANENJI_GRID_PRESENT_V","180"))
DISCHARGE_POWER_ON_W=float(os.getenv("ANENJI_DISCHARGE_POWER_ON_W","150"))
DISCHARGE_POWER_OFF_W=float(os.getenv("ANENJI_DISCHARGE_POWER_OFF_W","50"))
DISCHARGE_ON_DELAY_SEC=float(os.getenv("ANENJI_DISCHARGE_ON_DELAY_SEC","10"))
DISCHARGE_OFF_DELAY_SEC=float(os.getenv("ANENJI_DISCHARGE_OFF_DELAY_SEC","20"))
DISCHARGE_REQUIRE_GRID_ABSENT=os.getenv("ANENJI_DISCHARGE_REQUIRE_GRID_ABSENT","0")=="1"
RGB_CHARGE_DETECT_W=float(os.getenv("ANENJI_RGB_CHARGE_DETECT_W","20"))
ANJ_BMS={
 "module_full_charge_v":28.0,"module_ov_alarm_v":28.0,"module_ov_protection_v":29.2,
 "module_uv_alarm_v":23.2,"module_uv_protection_v":20.0,
 "cell_balance_start_v":3.4,"cell_balance_delta_mv":30.0,
 "cell_ov_alarm_v":3.5,"cell_ov_protection_v":3.65,
 "cell_uv_alarm_v":2.9,"cell_uv_protection_v":2.5,
 "charge_oc_alarm_a":105.0,"charge_oc_protection_a":110.0,
 "discharge_oc_alarm_a":105.0,"discharge_oc_protection_a":110.0,
 "temp_max_c":65.0,
}
MODE={0:"Power On",1:"Standby",2:"Mains",3:"Off-Grid",4:"Bypass",5:"Charging",6:"Fault"}

BATTERY_PROFILES={
    "2x_ANJ_STANDARD":{
        "label":"2× ANJ-25.6V-100AH-S — Standard 56.4V / 10A",
        "description":"2 batteries in series: 51.2 V nominal, 100 Ah, 5.12 kWh. USER mode. Standard daily profile: Bulk/CV 56.4 V total, below BMS hard OV protection. Charge current limited to 10 A; discharge limit 95 A.",
        "chemistry":"LiFePO4",
        "nominal_voltage":51.2,
        "capacity_ah":100,
        "energy_kwh":5.12,
        "charge_limit_a":10.0,
        "registers":{
            313:0,    # equalization OFF
            322:2,    # USER / USE
            324:564,  # 56.4 V Bulk/CV - standard profile below BMS hard OV protection
            325:540,  # 54.0 V Float
            327:460,  # 46.0 V Low DC protection in mains
            329:440,  # 44.0 V Low DC cut-off off-grid
            332:100,  # 10.0 A max charging current
            333:100,  # 10.0 A max utility charging current
            341:20,   # 20% low DC SOC in mains
            342:30,   # 30% recovery SOC
            343:15,   # 15% off-grid cut-off SOC
            351:95,  # 95 A max battery discharge current
        }
    },
    "2x_ANJ_LONG_LIFE":{
        "label":"2× ANJ-25.6V-100AH-S — Long Life 56.0V / 10A",
        "description":"2 batteries in series: 51.2 V nominal, 100 Ah, 5.12 kWh. USER mode. Long-life daily profile: Bulk/CV 56.0 V total, matching the BMS full-charge threshold of 28.0 V per module. Charge current limited to 10 A; discharge limit 80 A.",
        "chemistry":"LiFePO4",
        "nominal_voltage":51.2,
        "capacity_ah":100,
        "energy_kwh":5.12,
        "charge_limit_a":10.0,
        "registers":{
            313:0,    # equalization OFF
            322:2,    # USER / USE
            324:560,  # 56.0 V Bulk/CV - aligned with BMS pack full-charge threshold (28.0 V per module)
            325:540,  # 54.0 V Float
            327:460,  # 46.0 V Low DC protection in mains
            329:440,  # 44.0 V Low DC cut-off off-grid
            332:100,  # 10.0 A max charging current
            333:100,  # 10.0 A max utility charging current
            341:20,   # 20% low DC SOC in mains
            342:30,   # 30% recovery SOC
            343:15,   # 15% off-grid cut-off SOC
            351:80,  # 80 A max battery discharge current
        }
    }
}

SETTINGS={
300:dict(group="Basic",label="AC output mode",kind="select",values={0:"Single",1:"Parallel",2:"3P1 / L1",3:"3P2 / L2",4:"3P3 / L3"},safe=True),
301:dict(group="Basic",label="Main output priority",kind="select",values={1:"SUB — Solar → Utility → Battery",2:"SBU — Solar → Battery → Utility",3:"SUF — Solar → Utility → Battery / feed-in enabled",4:"ZEC — Zero Export to CT"},safe=True,note="raw 3 = SUF and raw 4 = ZEC confirmed experimentally on this inverter. raw 1=SUB and raw 2=SBU remain experimental mappings."),
302:dict(group="Other",label="Input voltage range",kind="select",values={0:"APL",1:"UPS",2:"GEN"},safe=True),
303:dict(group="Other",label="Buzzer mode",kind="select",values={0:"Mute",1:"Source/Warning/Fault",2:"Warning/Fault",3:"Fault only"},safe=True),
305:dict(group="Other",label="Backlight control",kind="select",values={0:"Off after 1 minute",1:"Always light"},safe=True),
306:dict(group="Other",label="LCD auto return",kind="select",values={0:"Disabled",1:"After 1 minute"},safe=True),
307:dict(group="Other",label="Power saving mode",kind="select",values={0:"Disabled",1:"Enable"},safe=True),
308:dict(group="Other",label="Auto restart when overload occurs",kind="select",values={0:"Prohibit",1:"Allow"},safe=True),
309:dict(group="Other",label="Auto restart when over-temperature occurs",kind="select",values={0:"Prohibit",1:"Allow"},safe=True),
310:dict(group="Other",label="Overload bypass",kind="select",values={0:"Prohibit",1:"Allow"},safe=True),
313:dict(group="Battery",label="Battery equalization",kind="select",values={0:"Disabled",1:"Enable"},safe=True),
320:dict(group="Basic",label="Output voltage setting",kind="select",values={2200:"220 V",2300:"230 V",2400:"240 V"},safe=True),
321:dict(group="Basic",label="Output frequency setting",kind="select",values={5000:"50 Hz",6000:"60 Hz"},safe=True),
322:dict(group="Battery",label="Battery type",kind="select",values={0:"AGM",1:"FLD",2:"USER / USE"},safe=True,note="Confirmed experimentally on this inverter: 0=AGM, 1=FLD, 2=USER/USE. LI1..LI4 are intentionally excluded from this selector because all tested LI modes take over RS485 for BMS communication and make this local Modbus interface unavailable."),
323:dict(group="Battery",label="Battery overvoltage protection",kind="number",scale=10,unit="V",min=48,max=68,step=.1,safe=True),
324:dict(group="Battery",label="Bulk charging voltage",kind="number",scale=10,unit="V",min=48,max=60,step=.1,safe=True),
325:dict(group="Battery",label="Floating charging voltage",kind="number",scale=10,unit="V",min=48,max=58,step=.1,safe=True),
326:dict(group="Battery",label="Recovery voltage back to mains mode",kind="number",scale=10,unit="V",min=40,max=60,step=.1,safe=True),
327:dict(group="Battery",label="Low DC protection voltage in mains mode",kind="number",scale=10,unit="V",min=40,max=55,step=.1,safe=True),
329:dict(group="Battery",label="Low DC protection voltage in off-grid mode",kind="number",scale=10,unit="V",min=40,max=55,step=.1,safe=True),
330:dict(group="Battery",label="Time from C.V to floating charge",kind="number",scale=1,unit="min",min=0,max=600,step=1,safe=True),
331:dict(group="Battery",label="Main charging priority",kind="select",values={1:"SOF — Solar first",2:"SNU — Solar + Utility",3:"OSO — Only Solar",4:"SOR — Solar residual"},safe=True,note="Confirmed experimentally on this firmware."),
332:dict(group="Battery",label="Maximum charging current",kind="number",scale=10,unit="A",min=0,max=120,step=.1,safe=True),
333:dict(group="Battery",label="Maximum utility charging current",kind="number",scale=10,unit="A",min=0,max=120,step=.1,safe=True),
334:dict(group="Battery",label="Equalization charging voltage",kind="number",scale=10,unit="V",min=48,max=62,step=.1,safe=True),
335:dict(group="Battery",label="Equalization time",kind="number",scale=1,unit="min",min=0,max=180,step=1,safe=True),
336:dict(group="Battery",label="Equalization timeout",kind="number",scale=1,unit="min",min=0,max=300,step=1,safe=True),
337:dict(group="Battery",label="Equalization interval",kind="number",scale=1,unit="day",min=1,max=90,step=1,safe=True),
341:dict(group="Battery SOC",label="Low DC protection SOC in mains mode",kind="number",scale=1,unit="%",min=0,max=100,step=1,safe=True),
342:dict(group="Battery SOC",label="Recovery SOC back to mains mode",kind="number",scale=1,unit="%",min=0,max=100,step=1,safe=True),
343:dict(group="Battery SOC",label="Low DC cut-off SOC in off-grid mode",kind="number",scale=1,unit="%",min=0,max=100,step=1,safe=True),
351:dict(group="Battery",label="Maximum battery discharge current",kind="number",scale=1,unit="A",min=0,max=500,step=1,safe=True,note="F2-27 confirmed: raw 0=OFF; enabled range 50..500 A; raw value equals amperes. Profiles use 95 A (Standard) / 80 A (Long Life)."),
344:dict(group="Basic",label="MAX feedback power to grid",kind="number",scale=1,unit="W",min=0,max=6200,step=100,safe=True,note="F1-05 confirmed writable: raw value equals watts. 1000 W write/read-back confirmed experimentally. Controls maximum grid export power."),
340:dict(group="Experimental",label="Unknown enum/state",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL; raw 3 repeatedly observed."),
349:dict(group="Experimental",label="Unknown parameter",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL; raw 100 observed."),
355:dict(group="Experimental",label="Unknown parameter A",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL; raw 90 observed."),
356:dict(group="Experimental",label="Unknown parameter B",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL; raw 6 observed."),
357:dict(group="Experimental",label="Unknown parameter C",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL; raw 100 observed."),
359:dict(group="Experimental",label="Possible shadow of Bulk voltage (324)",kind="number",scale=10,unit="V",min=0,max=6553.5,step=.1,safe=False,note="EXPERIMENTAL/read-only; possible duplicate of 324, not confirmed."),
360:dict(group="Experimental",label="Possible shadow of Float voltage (325)",kind="number",scale=10,unit="V",min=0,max=6553.5,step=.1,safe=False,note="EXPERIMENTAL/read-only; possible duplicate of 325, not confirmed."),
361:dict(group="Experimental",label="Possible shadow of Max charge current (332)",kind="number",scale=10,unit="A",min=0,max=6553.5,step=.1,safe=False,note="EXPERIMENTAL/read-only; possible duplicate of 332, not confirmed."),
406:dict(group="Remote",label="Turn-on mode",kind="select",values={0:"Local or remote",1:"Local only",2:"Remote only"},safe=True),
402:dict(group="Experimental",label="Possible shadow/state of Main output priority (301)",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL/read-only; candidate shadow of 301."),
403:dict(group="Experimental",label="Possible shadow/state of Main charging priority (331)",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL/read-only; candidate shadow of 331."),
404:dict(group="Experimental",label="Unknown enum/state",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL; raw 3 observed."),
421:dict(group="Experimental",label="Unknown parameter",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL; raw 120 observed."),
423:dict(group="Experimental",label="Unknown flag/state",kind="number",scale=1,unit="raw",min=0,max=65535,step=1,safe=False,note="EXPERIMENTAL; raw 1 observed."),
420:dict(group="Remote",label="Output control",kind="select",values={0:"OFF",1:"ON"},safe=False,note="Command register; locked."),
426:dict(group="Remote",label="Exit Fault Mode",kind="action",safe=False,note="Command candidate; locked."),
}

TELEMETRY={
201:("mode","Mode",None,False,""),202:("grid_voltage","Grid voltage",10,True,"V"),203:("grid_frequency","Grid frequency",100,True,"Hz"),
204:("grid_power","Grid power",1,True,"W"),205:("inverter_voltage","Inverter voltage",10,True,"V"),206:("inverter_current","Inverter current",10,True,"A"),
207:("inverter_frequency","Inverter frequency",100,True,"Hz"),208:("inverter_power","Inverter power",1,True,"W"),209:("inverter_charging_power","Inverter charging power",1,True,"W"),
210:("output_voltage","Output voltage",10,True,"V"),211:("output_current","Output current",10,True,"A"),212:("output_frequency","Output frequency",100,True,"Hz"),
213:("output_active_power","Output active power",1,True,"W"),214:("output_apparent_power","Output apparent power",1,True,"VA"),215:("battery_voltage","Battery voltage",10,True,"V"),
216:("battery_current","Battery current",10,True,"A"),217:("battery_power","Battery power",1,True,"W"),219:("pv_voltage","PV voltage",10,True,"V"),
220:("pv_current","PV current",10,True,"A"),223:("pv_power","PV power",1,True,"W"),224:("pv_charging_power","PV charging power",1,True,"W"),
225:("load_percent","Load",1,True,"%"),226:("dcdc_temperature","DCDC temperature",1,True,"°C"),227:("inverter_temperature","Inverter temperature",1,True,"°C"),
229:("battery_soc","Battery SOC",1,False,"%"),232:("net_battery_current","Net battery current",10,True,"A"),233:("inverter_charge_current","Inverter charge current",10,True,"A"),
234:("pv_charge_current","PV charge current",10,True,"A"),
}
OBSERVED={200:"Status/capability word",218:"Internal/unknown",221:"Reserved/unknown",222:"Reserved/unknown",228:"Reserved/unknown",230:"Reserved/unknown",231:"Reserved/unknown",235:"Internal/unknown",236:"Internal/unknown",237:"Reserved/unknown",238:"Internal/unknown",239:"Reserved/unknown",304:"Unknown",311:"Unknown",312:"Unknown",314:"Unknown",315:"Unknown",316:"Unknown",317:"Unknown",318:"Unknown",319:"Unknown",328:"Unknown",338:"Unknown",339:"Unknown",340:"Unknown (raw 3 observed)",349:"Unknown (raw 100 observed)",351:"Maximum battery discharge current",355:"Unknown (raw 90 observed)",356:"Unknown (raw 6 observed)",357:"Unknown (raw 100 observed)",359:"EXPERIMENTAL: possible shadow/duplicate of Bulk voltage (324)",360:"EXPERIMENTAL: possible shadow/duplicate of Float voltage (325)",361:"EXPERIMENTAL: possible shadow/duplicate of Max charge current (332)",402:"EXPERIMENTAL: possible shadow/state of Main output priority (301)",403:"EXPERIMENTAL: possible shadow/state of Main charging priority (331)",404:"EXPERIMENTAL: unknown enum/state (raw 3 observed)",421:"EXPERIMENTAL: unknown parameter (raw 120 observed)",423:"EXPERIMENTAL: unknown flag/state (raw 1 observed)",434:"RTC year",435:"RTC month",436:"RTC day",437:"RTC hour",438:"RTC minute",439:"RTC second"}
KNOWN=set(TELEMETRY)|set(SETTINGS)|set(OBSERVED)

def crc16(data):
    crc=0xffff
    for b in data:
        crc^=b
        for _ in range(8): crc=(crc>>1)^0xA001 if crc&1 else crc>>1
    return crc
def s16(v): return v-65536 if v>=0x8000 else v

class Bus:
    def __init__(self):
        self.lock=threading.RLock();self.ser=serial.Serial(PORT,BAUD,bytesize=8,parity="N",stopbits=1,timeout=.7)
        self.tx=self.rx=self.errors=self.timeouts=0;self.last_error="";self.last_ok=0.0
    def close(self):
        try:self.ser.close()
        except:pass
    def _read_exact(self,n):
        data=bytearray()
        deadline=time.monotonic()+self.ser.timeout+0.25
        while len(data)<n and time.monotonic()<deadline:
            chunk=self.ser.read(n-len(data))
            if chunk:data.extend(chunk)
        return bytes(data)

    def exchange(self,pdu,retries=3):
        frame=bytes([SLAVE])+pdu
        c=crc16(frame);frame+=bytes([c&255,c>>8])
        last=None
        with self.lock:
            for attempt in range(retries):
                try:
                    self.ser.reset_input_buffer()
                    self.ser.write(frame);self.ser.flush();self.tx+=1

                    # Read RTU response deterministically instead of read(256).
                    head=self._read_exact(2)
                    if len(head)<2:
                        self.timeouts+=1
                        raise RuntimeError("No/short Modbus response")

                    fn=head[1]
                    if fn&0x80:
                        tail=self._read_exact(3)  # exception code + CRC
                        r=head+tail
                    elif fn in (0x03,0x04):
                        bc=self._read_exact(1)
                        if len(bc)!=1:
                            self.timeouts+=1
                            raise RuntimeError("Short Modbus byte-count")
                        tail=self._read_exact(bc[0]+2)
                        r=head+bc+tail
                    elif fn in (0x06,0x10):
                        tail=self._read_exact(6)
                        r=head+tail
                    else:
                        # Unknown function: fall back to a short idle-gap read.
                        time.sleep(.03)
                        r=head+self.ser.read(254)

                    if len(r)<5:
                        self.timeouts+=1
                        raise RuntimeError("No/short Modbus response")
                    self.rx+=1
                    if (r[-2]|r[-1]<<8)!=crc16(r[:-2]):
                        raise RuntimeError("CRC error")
                    if r[0]!=SLAVE:
                        raise RuntimeError("Unexpected slave")
                    if r[1]&0x80:
                        raise RuntimeError("Modbus exception 0x%02X"%r[2])

                    self.last_ok=time.time();self.last_error=""
                    return r
                except Exception as e:
                    last=e;self.errors+=1;self.last_error=str(e)
                    try:self.ser.reset_input_buffer()
                    except:pass
                    time.sleep(.12*(attempt+1))
        raise last

    def read(self,a,n=1):
        r=self.exchange(bytes([3,a>>8,a&255,n>>8,n&255]))
        d=r[3:3+r[2]]
        if len(d)!=n*2:raise RuntimeError("Unexpected byte count")
        return [(d[i]<<8)|d[i+1] for i in range(0,len(d),2)]

    def read_chunked(self,a,n,chunk=20):
        out=[]
        pos=0
        while pos<n:
            c=min(chunk,n-pos)
            out.extend(self.read(a+pos,c))
            pos+=c
            time.sleep(.02)
        return out
    def write_block_fc16(self,a,values):
        if not ALLOW_WRITE: raise PermissionError("Global writes disabled")
        if not values or len(values)>20: raise ValueError("Invalid block length")
        payload=b"".join(bytes([v>>8,v&255]) for v in values)
        n=len(values)
        p=bytes([0x10,a>>8,a&255,n>>8,n&255,len(payload)])+payload
        r=self.exchange(p)
        if len(r)<8 or r[1]!=0x10: raise RuntimeError("Unexpected FC16 response")
        if ((r[2]<<8)|r[3])!=a or ((r[4]<<8)|r[5])!=n: raise RuntimeError("FC16 response mismatch")
        time.sleep(.15)
        q=self.read(a,n)
        if q!=values: raise RuntimeError("RTC write verify failed")
        return q

    def write_fc16(self,a,v):
        meta=SETTINGS.get(a)
        if not ALLOW_WRITE:raise PermissionError("Global writes disabled")
        if not meta or not meta.get("safe"):raise PermissionError("Register locked")
        p=bytes([0x10,a>>8,a&255,0,1,2,v>>8,v&255]);r=self.exchange(p)
        if len(r)<8 or r[1]!=0x10:raise RuntimeError("Unexpected FC16 response")
        if ((r[2]<<8)|r[3])!=a or ((r[4]<<8)|r[5])!=1:raise RuntimeError("FC16 response mismatch")
        time.sleep(.12);q=self.read(a)[0]
        if q!=v:raise RuntimeError("Write verify failed")
        return q

BUS=None;CACHE_LOCK=threading.RLock()
CACHE={"online":False,"updated":0,"status":[],"settings":[],"rtc":None,"error":"","settings_error":"","rtc_error":""}
HISTORY=deque(maxlen=HISTORY_LEN)

BMS_LOCK=threading.RLock()
BMS_PARTICIPATION={}
BMS_PARTICIPATION_LOCK=threading.Lock()
BMS_AUTO_LOCK=threading.Lock()
BMS_AUTO_STATE={
    "was_discharging":False,
    "post_remaining":0,
    "last_post_request":0.0,
    "recovered_at":0.0,
}
ATTENTION_LOCK=threading.Lock()
ATTENTION_STATE={"required":False,"condition_since":0.0,"clear_since":0.0,"changed_at":0.0}
DISCHARGE_LOCK=threading.Lock()
DISCHARGE_STATE={"detected":False,"on_since":0.0,"off_since":0.0,"changed_at":0.0}
BMS_CACHE={"enabled":BMS_ENABLE,"available":False,"online":False,"updated":0,"devices":[],"aggregate":{},"error":"","poll_sec":BMS_POLL_SEC,"expected_packs":BMS_EXPECTED_PACKS,"fail_threshold":BMS_FAIL_THRESHOLD,"stale_after_sec":BMS_STALE_AFTER_SEC,"polling":False,"last_started":0,"last_finished":0,"warn_age_sec":BMS_WARN_AGE_SEC,"old_age_sec":BMS_OLD_AGE_SEC,"cycle_started":0,"cycle_finished":0,"cycle_duration_sec":0,"parallel":BMS_PARALLEL,"retry_failed":BMS_RETRY_FAILED}
BMS_LAST_GOOD={}
BMS_FAILS={}

def _bms_run(args,timeout):
    p=subprocess.run(args,text=True,capture_output=True,timeout=timeout)
    if p.returncode!=0:
        raise RuntimeError((p.stderr or "").strip() or ("exit %d"%p.returncode))
    return json.loads(p.stdout)

def _bms_read_one(exe,mac):
    try:
        tout=max(6.0,(BMS_IO_TIMEOUT_MS/1000.0)+3.0)
        d=_bms_run([exe,"-T",str(BMS_IO_TIMEOUT_MS),"read",mac],tout)
        if isinstance(d,dict):
            d["_poll_ok"]=True
            d["_last_attempt"]=time.time()
            return d
        return {"address":mac,"ok":False,"_poll_ok":False,"_last_attempt":time.time(),"error":"Invalid BMS response"}
    except Exception as e:
        return {"address":mac,"ok":False,"_poll_ok":False,"_last_attempt":time.time(),"error":str(e)}

def _bms_read_devices():
    if not BMS_ENABLE:return []
    exe=shutil.which(BMS_CMD) if "/" not in BMS_CMD else (BMS_CMD if os.path.exists(BMS_CMD) else None)
    if not exe:raise RuntimeError("%s not found"%BMS_CMD)
    if not BMS_MACS:
        raise RuntimeError("ANENJI_BMS_MACS is not configured; automatic read-all scan is disabled")

    results={}
    order=list(BMS_MACS)
    width=max(1,min(BMS_PARALLEL,len(order)))

    def run_batch(batch):
        tmp={}
        lock=threading.Lock()
        threads=[]
        def worker(mac):
            r=_bms_read_one(exe,mac)
            with lock:tmp[mac]=r
        for mac in batch:
            th=threading.Thread(target=worker,args=(mac,),daemon=True)
            threads.append(th); th.start()
        join_timeout=max(8.0,(BMS_IO_TIMEOUT_MS/1000.0)+5.0)
        for th in threads: th.join(join_timeout)
        for mac in batch:
            tmp.setdefault(mac,{"address":mac,"ok":False,"_poll_ok":False,"_last_attempt":time.time(),"error":"BMS worker timed out"})
        return tmp

    for pos in range(0,len(order),width):
        batch=order[pos:pos+width]
        results.update(run_batch(batch))

    # Retry only failed modules. Successful BMS units are not queried twice.
    for _ in range(BMS_RETRY_FAILED):
        failed=[mac for mac in order if not results.get(mac,{}).get("_poll_ok")]
        if not failed:break
        if BMS_RETRY_DELAY_SEC>0:time.sleep(BMS_RETRY_DELAY_SEC)
        for pos in range(0,len(failed),width):
            batch=failed[pos:pos+width]
            results.update(run_batch(batch))

    return [results[mac] for mac in order]

def _bms_merge_health(current):
    now=time.time()
    merged=[]
    for item in current:
        addr=(item.get("device") or {}).get("address") or item.get("address") or "unknown"
        if item.get("_poll_ok") and item.get("pack"):
            BMS_LAST_GOOD[addr]=json.loads(json.dumps(item))
            BMS_LAST_GOOD[addr]["_last_success"]=now
            BMS_FAILS[addr]=0
            item["_consecutive_failures"]=0
            item["_stale"]=False
            item["_online"]=True
            merged.append(item)
            continue

        BMS_FAILS[addr]=BMS_FAILS.get(addr,0)+1
        last=BMS_LAST_GOOD.get(addr)
        if last:
            x=json.loads(json.dumps(last))
            last_success=float(x.get("_last_success",0))
            age=max(0,now-last_success)
            x["_poll_ok"]=False
            x["_consecutive_failures"]=BMS_FAILS[addr]
            x["_last_attempt"]=item.get("_last_attempt",now)
            x["_last_success"]=last_success
            x["_stale"]=age>BMS_STALE_AFTER_SEC
            x["_online"]=BMS_FAILS[addr] < BMS_FAIL_THRESHOLD and not x["_stale"]
            x["_error"]=item.get("error","read failed")
            merged.append(x)
        else:
            item["_consecutive_failures"]=BMS_FAILS[addr]
            item["_stale"]=True
            item["_online"]=False
            merged.append(item)
    return merged

def _bms_aggregate(devices):
    ok=[d for d in devices if isinstance(d,dict) and d.get("pack") and d.get("cells")]
    if not ok:return {}
    volts=[float(d["pack"].get("voltage_v",0)) for d in ok]
    currents=[float(d["pack"].get("current_a",0)) for d in ok]
    powers=[float(d["pack"].get("power_w",0)) for d in ok]
    socs=[float(d["pack"].get("soc_pct",0)) for d in ok]
    sohs=[float(d["pack"].get("soh_pct",0)) for d in ok]
    deltas=[float(d["cells"].get("delta_mv",0)) for d in ok]
    cmins=[float(d["cells"].get("min_v",0)) for d in ok]
    cmaxs=[float(d["cells"].get("max_v",0)) for d in ok]
    tmax=[float(d.get("temperatures",{}).get("overall_max_c",0)) for d in ok]
    problems=[bool(d.get("status",{}).get("problem")) for d in ok]
    balancing=[bool(d.get("status",{}).get("balancing_active")) for d in ok]
    remwh=[float(d.get("capacity",{}).get("remaining_wh_est",0)) for d in ok]
    designwh=[float(d.get("capacity",{}).get("design_wh_at_present_voltage",0)) for d in ok]
    cycles=[int(d.get("capacity",{}).get("cycles",0)) for d in ok]
    a={
      "count":len(ok),
      "topology":BMS_TOPOLOGY,
      "pack_voltage_min_v":min(volts),"pack_voltage_max_v":max(volts),
      "pack_voltage_avg_v":sum(volts)/len(volts),
      "pack_voltage_spread_v":max(volts)-min(volts) if len(volts)>1 else 0,
      "pack_current_min_a":min(currents),"pack_current_max_a":max(currents),
      "pack_current_avg_a":sum(currents)/len(currents),
      "current_spread_a":max(currents)-min(currents) if len(currents)>1 else 0,
      "soc_min_pct":min(socs),"soc_max_pct":max(socs),"soc_avg_pct":sum(socs)/len(socs),
      "soc_spread_pct":max(socs)-min(socs) if len(socs)>1 else 0,
      "soh_min_pct":min(sohs),"remaining_wh_est":sum(remwh),"design_wh_est":sum(designwh),
      "cell_min_v":min(cmins),"cell_max_v":max(cmaxs),"cell_delta_max_mv":max(deltas),
      "temperature_max_c":max(tmax),"problem":any(problems),
      "balancing_active":any(balancing),"balancing_packs":sum(1 for x in balancing if x),
      "cycles_max":max(cycles) if cycles else 0,
    }
    # Optional derived electrical totals only when the topology is explicitly configured.
    # Supported examples:
    #   4s   = four 25.6 V modules in series
    #   2s2p = two series modules per string, two parallel strings
    if BMS_TOPOLOGY=="4s" and len(ok)==4:
        a["derived_battery_voltage_v"]=sum(volts)
        a["derived_battery_current_a"]=sum(currents)/len(currents)  # same series current, BLE samples asynchronous
        a["derived_battery_power_w"]=sum(powers)
    elif BMS_TOPOLOGY=="2s2p" and len(ok)==4:
        # Cannot know physical pair membership from BLE addresses alone, therefore
        # do not derive string voltage/current until grouping is explicitly implemented.
        a["topology_note"]="2s2p selected, but pack-to-string grouping is not configured; no electrical totals derived"
    return a

def _settings_raw_map():
    with CACHE_LOCK:return {int(x["register"]):int(x["raw"]) for x in CACHE.get("settings",[]) if "register" in x and "raw" in x}

def bms_poll_once():
    cycle_started=time.time()
    with BMS_LOCK:
        if BMS_CACHE.get("polling"):
            return False
        BMS_CACHE["polling"]=True
        BMS_CACHE["last_started"]=cycle_started
        BMS_CACHE["cycle_started"]=cycle_started
    try:
        raw=_bms_read_devices()
        d=_bms_merge_health(raw)
        a=_bms_aggregate(d)
        now=time.time()
        healthy=sum(1 for x in d if x.get("_online"))
        stale=sum(1 for x in d if x.get("_stale"))
        with BMS_LOCK:
            BMS_CACHE.update(available=True,online=healthy>0,updated=now,devices=d,aggregate=a,
                healthy_count=healthy,stale_count=stale,error="",last_finished=now,
                cycle_finished=now,cycle_duration_sec=max(0,now-cycle_started),parallel=BMS_PARALLEL,retry_failed=BMS_RETRY_FAILED)
        return True
    except Exception as e:
        now=time.time()
        with BMS_LOCK:
            BMS_CACHE.update(available=bool(shutil.which(BMS_CMD)),online=bool(BMS_CACHE.get("aggregate")),
                error=str(e),last_finished=now,cycle_finished=now,cycle_duration_sec=max(0,now-cycle_started))
        return False
    finally:
        with BMS_LOCK:BMS_CACHE["polling"]=False

def bms_trigger_async():
    with BMS_LOCK:
        if BMS_CACHE.get("polling"):
            return False
        BMS_CACHE["polling"]=True
        BMS_CACHE["last_started"]=time.time()
    def _run():
        with BMS_LOCK:
            BMS_CACHE["polling"]=False
        bms_poll_once()
    threading.Thread(target=_run,daemon=True).start()
    return True

def bms_worker():
    # The initial BMS request is launched once from main().
    # Here we wait one full interval before the next automatic refresh.
    # BMS_POLL_SEC == 0 means this worker is not started.
    while True:
        time.sleep(max(5.0,BMS_POLL_SEC))
        bms_trigger_async()


def decode_status(vals):
    out=[];bykey={}
    for a,(key,label,scale,signed,unit) in TELEMETRY.items():
        raw=vals[a-200];v=s16(raw) if signed else raw
        if a==201:v=MODE.get(raw,"Unknown (%d)"%raw)
        elif scale:v=v/scale
        item=dict(key=key,register=a,label=label,value=v,raw=raw,unit=unit);out.append(item);bykey[key]=v
    return out,bykey

def read_settings_blocks():
    """
    Read settings without letting one unsupported/intermittent register hide the
    entire Settings UI.

    The main ranges are read in chunks. If a range fails, fall back to reading
    only known SETTINGS addresses from that range one by one. Optional 400-series
    registers are always read individually and failures are ignored.
    """
    b={}
    known=sorted(SETTINGS)

    for start,count in ((300,38),(340,22)):
        try:
            vals=BUS.read_chunked(start,count,20)
            for i,v in enumerate(vals):
                b[start+i]=v
        except Exception:
            # Fallback: salvage all known settings from this range.
            for a in (x for x in known if start<=x<start+count):
                try:
                    b[a]=BUS.read(a)[0]
                except Exception:
                    pass

    # Optional/experimental 400-series settings must never break the whole list.
    for a in (x for x in known if x>=400):
        try:
            b[a]=BUS.read(a)[0]
        except Exception:
            pass

    return b

def decode_settings(b):
    out=[]
    for a,meta in SETTINGS.items():
        if a not in b:
            item=dict(
                register=a,raw=None,value=None,display="Unavailable",
                read_ok=False,write_enabled=False,**meta
            )
            out.append(item)
            continue

        raw=b[a]
        if meta["kind"]=="action":
            out.append(dict(register=a,raw=raw,value=raw,display="Command",read_ok=True,write_enabled=False,**meta))
            continue

        if meta["kind"]=="select":
            val=raw
            disp=meta["values"].get(raw,"Unknown (%d)"%raw)
        else:
            val=raw/meta.get("scale",1)
            disp=("OFF" if a==351 and raw==0 else ("%g %s"%(val,meta.get("unit",""))).strip())

        out.append(dict(
            register=a,raw=raw,value=val,display=disp,read_ok=True,
            write_enabled=ALLOW_WRITE and meta.get("safe",False),**meta
        ))
    return out

def read_rtc():
    v=BUS.read_chunked(434,6,6)
    return "%04d-%02d-%02d %02d:%02d:%02d"%tuple(v)

def _history_energy_window(window_sec=None):
    """Signed average battery power from inverter history.
    Positive = discharge, negative = charge.
    """
    if window_sec is None:window_sec=ETA_AVG_SEC
    now=time.time()
    with CACHE_LOCK:rows=list(HISTORY)
    vals=[]
    for r in rows:
        try:
            ts=float(r.get("ts") or 0)
            if ts and now-ts>window_sec:continue
            v=float(r.get("battery_voltage"))
            a=float(r.get("net_battery_current"))
            if v>0:vals.append(-(v*a))
        except (TypeError,ValueError):
            pass
    if not vals:return {"avg_w":None,"samples":0,"window_sec":window_sec}
    return {"avg_w":sum(vals)/len(vals),"samples":len(vals),"window_sec":window_sec,
            "min_w":min(vals),"max_w":max(vals)}

def _bms_pack_id(d):
    dev=d.get("device",{})
    return str(dev.get("name") or dev.get("address") or "unknown")

def _bms_discharge_current(d):
    try:a=float(d.get("pack",{}).get("current_a",0) or 0)
    except (TypeError,ValueError):a=0.0
    # Saihang samples observed in this installation: positive=charge, negative=discharge.
    return max(0.0,-a)

def _classify_bms_participation(devices,bank_discharging):
    now=time.time();raw=[];rows=[]
    by_addr={}
    for d in devices:
        if not isinstance(d,dict):
            continue
        if isinstance(d.get("data"),dict):
            x=d["data"]
            poll_ok=bool(d.get("ok",True))
        else:
            x=d
            poll_ok=bool(d.get("_poll_ok",d.get("_online",False)))

        dev=x.get("device",{}) if isinstance(x,dict) else {}
        addr=str(dev.get("address") or d.get("address") or "")
        if addr:
            by_addr[addr.upper()]=(x,poll_ok)

    # Use the configured MAC list as the authoritative expected bank membership.
    expected=[m.upper() for m in BMS_MACS] if BMS_MACS else list(by_addr.keys())

    for mac in expected:
        x,poll_ok=by_addr.get(mac,({},False))
        if poll_ok and isinstance(x.get("pack"),dict):
            pid=_bms_pack_id(x)
            raw.append((mac,pid,x,_bms_discharge_current(x),True))
        else:
            # Preserve configured MAC even when it did not answer this cycle.
            raw.append((mac,mac,x if isinstance(x,dict) else {},0.0,False))

    any_active=bank_discharging and any(ok and a>=BMS_ACTIVE_CURRENT_A for _,_,_,a,ok in raw)

    with BMS_PARTICIPATION_LOCK:
        for mac,pid,x,dis_a,poll_ok in raw:
            st=BMS_PARTICIPATION.setdefault(
                mac,{"inactive_cycles":0,"offline_cycles":0,"ever_active":False,"last_active":0.0}
            )

            active=bool(bank_discharging and poll_ok and dis_a>=BMS_ACTIVE_CURRENT_A)

            if active:
                st["inactive_cycles"]=0
                st["offline_cycles"]=0
                st["ever_active"]=True
                st["last_active"]=now
            elif not poll_ok:
                st["offline_cycles"]=int(st.get("offline_cycles",0))+1
                if bank_discharging:
                    st["inactive_cycles"]=int(st.get("inactive_cycles",0))+1
            elif bank_discharging and any_active:
                st["offline_cycles"]=0
                st["inactive_cycles"]=int(st.get("inactive_cycles",0))+1
            else:
                st["offline_cycles"]=0
                st["inactive_cycles"]=0

            if not poll_ok:
                state="offline"
            elif active:
                state="active"
            elif bank_discharging and any_active and st["inactive_cycles"]>=BMS_INACTIVE_CYCLES:
                state="suspect"
            else:
                state="idle"

            rows.append({
                "mac":mac,
                "id":pid,
                "state":state,
                "discharge_current_a":dis_a,
                "inactive_cycles":st["inactive_cycles"],
                "offline_cycles":st["offline_cycles"],
                "ever_active":bool(st.get("ever_active")),
                "data":x
            })
    return rows

def _update_discharge_state(discharge_power_w,now=None):
    if now is None:now=time.time()
    p=max(0.0,float(discharge_power_w or 0.0))
    with DISCHARGE_LOCK:
        st=DISCHARGE_STATE
        if not st["detected"]:
            st["off_since"]=0.0
            if p>=DISCHARGE_POWER_ON_W:
                if not st["on_since"]:st["on_since"]=now
                if now-st["on_since"]>=DISCHARGE_ON_DELAY_SEC:
                    st["detected"]=True;st["changed_at"]=now;st["off_since"]=0.0
            else:
                st["on_since"]=0.0
        else:
            st["on_since"]=0.0
            if p<=DISCHARGE_POWER_OFF_W:
                if not st["off_since"]:st["off_since"]=now
                if now-st["off_since"]>=DISCHARGE_OFF_DELAY_SEC:
                    st["detected"]=False;st["changed_at"]=now;st["off_since"]=0.0
            else:
                st["off_since"]=0.0
        return {
            "discharge_detected":bool(st["detected"]),
            "discharge_detected_since":st["changed_at"] if st["detected"] else None,
            "discharge_on_pending_sec":max(0.0,now-st["on_since"]) if st["on_since"] and not st["detected"] else 0.0,
            "discharge_off_pending_sec":max(0.0,now-st["off_since"]) if st["off_since"] and st["detected"] else 0.0,
        }

def _update_attention_state(raw_condition,now=None):
    if now is None:now=time.time()
    with ATTENTION_LOCK:
        st=ATTENTION_STATE
        if not ATTENTION_ENABLE:
            st.update(required=False,condition_since=0.0,clear_since=0.0)
            return {"attention_required":False,"attention_pending":False,"attention_pending_sec":0.0,
                    "attention_clear_pending":False,"attention_clear_pending_sec":0.0,
                    "attention_changed_at":st.get("changed_at",0.0)}

        if raw_condition:
            st["clear_since"]=0.0
            if not st["condition_since"]:st["condition_since"]=now
            elapsed=max(0.0,now-st["condition_since"])
            if not st["required"] and elapsed>=ATTENTION_ON_DELAY_SEC:
                st["required"]=True
                st["changed_at"]=now
            return {"attention_required":bool(st["required"]),
                    "attention_pending":not bool(st["required"]),
                    "attention_pending_sec":elapsed if not st["required"] else 0.0,
                    "attention_clear_pending":False,"attention_clear_pending_sec":0.0,
                    "attention_changed_at":st.get("changed_at",0.0)}

        st["condition_since"]=0.0
        if st["required"]:
            if not st["clear_since"]:st["clear_since"]=now
            elapsed=max(0.0,now-st["clear_since"])
            if elapsed>=ATTENTION_OFF_DELAY_SEC:
                st["required"]=False
                st["clear_since"]=0.0
                st["changed_at"]=now
                elapsed=0.0
            return {"attention_required":bool(st["required"]),
                    "attention_pending":False,"attention_pending_sec":0.0,
                    "attention_clear_pending":bool(st["required"]),
                    "attention_clear_pending_sec":elapsed if st["required"] else 0.0,
                    "attention_changed_at":st.get("changed_at",0.0)}

        st["clear_since"]=0.0
        return {"attention_required":False,"attention_pending":False,"attention_pending_sec":0.0,
                "attention_clear_pending":False,"attention_clear_pending_sec":0.0,
                "attention_changed_at":st.get("changed_at",0.0)}

def energy_summary():
    now=time.time()
    with CACHE_LOCK:
        status=list(CACHE.get("status",[]));inverter_updated=float(CACHE.get("updated",0) or 0)
    sm={x.get("key"):x.get("value") for x in status}
    def num(k,d=0.0):
        try:return float(sm.get(k,d))
        except:return float(d)
    bv=num("battery_voltage");ba=num("net_battery_current",num("battery_current"));inv_soc=num("battery_soc",-1)
    batt_w=-(bv*ba) if bv>0 else 0.0
    grid_v=num("grid_voltage")
    grid_present=bool(grid_v>=GRID_PRESENT_V)
    discharge_input_w=batt_w
    if DISCHARGE_REQUIRE_GRID_ABSENT and grid_present:
        discharge_input_w=0.0
    discharge_state=_update_discharge_state(discharge_input_w,now)
    pv_w=num("pv_power")
    load_w=num("output_active_power")
    grid_w=num("grid_power")
    hist=_history_energy_window(ETA_AVG_SEC);avg_w=hist.get("avg_w")
    avg_for_eta=avg_w if avg_w is not None else batt_w
    bank_discharging=bool(discharge_state.get("discharge_detected"))

    with BMS_LOCK:
        bms_updated=float(BMS_CACHE.get("updated",0) or 0)
        devices=json.loads(json.dumps(BMS_CACHE.get("devices") or []))
        healthy=int(BMS_CACHE.get("healthy_count",0) or 0);total=len(BMS_CACHE.get("devices") or [])

    packs=_classify_bms_participation(devices,bank_discharging)
    active=[p for p in packs if p["state"]=="active"]
    suspect=[p for p in packs if p["state"]=="suspect"]
    offline=[p for p in packs if p["state"]=="offline"]
    rem_wh=sum(float(p["data"].get("capacity",{}).get("remaining_wh_est",0) or 0) for p in active)
    design_wh=sum(float(p["data"].get("capacity",{}).get("design_wh_at_present_voltage",0) or 0) for p in active)
    reserve=max(0.0,min(100.0,BATTERY_RESERVE_SOC))
    usable=max(0.0,rem_wh-design_wh*reserve/100.0) if design_wh else 0.0
    eta_power=avg_for_eta if avg_for_eta is not None and avg_for_eta>ETA_MIN_POWER_W else batt_w
    eta=(usable/eta_power*3600.0) if bank_discharging and usable>0 and eta_power>0 else None

    codes=[];problem_packs=[];level="none"
    for p in packs:
        d=p["data"];pid=p["id"]
        if d.get("status",{}).get("problem"):
            codes.append("BMS_REPORTED_PROBLEM");problem_packs.append(pid);level="critical"
        try:temp=float(d.get("temperatures",{}).get("overall_max_c",0) or 0)
        except:temp=0
        if temp>=BMS_CRITICAL_TEMP_C:
            codes.append("HIGH_BATTERY_TEMPERATURE");problem_packs.append(pid);level="critical"
    if suspect:
        codes.append("PACK_NOT_PARTICIPATING")
        problem_packs += [p["id"] for p in suspect]
        if level!="critical":level="warning"
    if offline:
        codes.append("BMS_OFFLINE")
        problem_packs += [p["id"] for p in offline]
        if level=="none":level="warning"
    age=(now-bms_updated) if bms_updated else None
    quality="no_bms" if not bms_updated else ("fresh" if age<BMS_WARN_AGE_SEC else ("old" if age>=BMS_OLD_AGE_SEC else "stale"))
    if quality=="old":
        codes.append("BMS_DATA_STALE")
        if level=="none":level="warning"
    codes=list(dict.fromkeys(codes));problem_packs=list(dict.fromkeys(problem_packs))
    deltas=[];temps=[]
    for p in packs:
        try:deltas.append(float(p["data"].get("cells",{}).get("delta_mv")))
        except:pass
        try:temps.append(float(p["data"].get("temperatures",{}).get("overall_max_c")))
        except:pass
    battery_discharging=bool(discharge_state.get("discharge_detected"))
    power_deficit_w=max(0.0,load_w-pv_w)
    attention_grid_ok=(not grid_present) if ATTENTION_REQUIRE_GRID_ABSENT else True
    attention_condition=bool(
        ATTENTION_ENABLE and
        attention_grid_ok and
        battery_discharging and
        power_deficit_w>=ATTENTION_BATTERY_POWER_W
    )
    attention=_update_attention_state(attention_condition,now)

    return {"timestamp":int(now),"inverter_age_sec":now-inverter_updated if inverter_updated else None,
      "state":"discharging" if bank_discharging else ("charging" if avg_for_eta is not None and avg_for_eta<=-ETA_MIN_POWER_W else "idle"),
      "pv_w":pv_w,"load_w":load_w,"grid_w":grid_w,"grid_voltage_v":grid_v,
      "grid_present":grid_present,"battery_discharging":battery_discharging,
      **discharge_state,
      "discharge_power_w":max(0.0,batt_w),
      "power_deficit_w":power_deficit_w,
      "attention_require_grid_absent":ATTENTION_REQUIRE_GRID_ABSENT,
      "attention_grid_ok":attention_grid_ok,
      "attention_condition":attention_condition,**attention,
      "battery_voltage_v":bv,"battery_current_a":ba,"battery_power_w":batt_w,"battery_soc_pct":inv_soc,"battery_avg_power_w":avg_w,
      "eta_window_sec":ETA_AVG_SEC,"eta_samples":hist.get("samples",0),"reserve_soc_pct":reserve,
      "bms_active":len(active),"bms_online":healthy,"bms_total":len(packs),"bms_expected":len(packs),
      "active_packs":[p["id"] for p in active],
      "idle_packs":[p["id"] for p in packs if p["state"]=="idle"],
      "suspect_packs":[p["id"] for p in suspect],
      "offline_packs":[p["id"] for p in offline],
      "active_remaining_wh":rem_wh,
      "active_design_wh_est":design_wh,"active_usable_to_reserve_wh":usable,"eta_to_reserve_sec":eta,
      "eta_to_full_sec":None,"bms_age_sec":age,"bms_quality":quality,
      "max_cell_delta_mv":max(deltas) if deltas else None,"max_battery_temp_c":max(temps) if temps else None,
      "battery_problem":bool(codes),"battery_problem_level":level,"battery_problem_codes":codes,
      "battery_problem_packs":problem_packs,
      "pack_participation":[{k:p[k] for k in ("mac","id","state","discharge_current_a","inactive_cycles","offline_cycles","ever_active")} for p in packs],
      "active_current_threshold_a":BMS_ACTIVE_CURRENT_A,
      "inactive_cycles_threshold":BMS_INACTIVE_CYCLES,
      "attention_enabled":ATTENTION_ENABLE,
      "attention_on_delay_sec":ATTENTION_ON_DELAY_SEC,
      "attention_off_delay_sec":ATTENTION_OFF_DELAY_SEC,
      "attention_battery_power_w":ATTENTION_BATTERY_POWER_W,
      "attention_require_grid_absent":ATTENTION_REQUIRE_GRID_ABSENT,
      "grid_present_threshold_v":GRID_PRESENT_V,
      "discharge_power_on_w":DISCHARGE_POWER_ON_W,
      "discharge_power_off_w":DISCHARGE_POWER_OFF_W,
      "discharge_on_delay_sec":DISCHARGE_ON_DELAY_SEC,
      "discharge_off_delay_sec":DISCHARGE_OFF_DELAY_SEC,
      "discharge_require_grid_absent":DISCHARGE_REQUIRE_GRID_ABSENT}


def maybe_trigger_bms_on_discharge(bykey):
    if not (BMS_ENABLE and BMS_AUTO_ON_DISCHARGE):return
    try:
        v=float(bykey.get("battery_voltage") or 0)
        a=float(bykey.get("net_battery_current",bykey.get("battery_current")) or 0)
    except (TypeError,ValueError):
        return

    now=time.time()
    discharge_w=max(0.0,-(v*a))
    try:
        gv=float(bykey.get("grid_voltage") or 0)
    except (TypeError,ValueError):
        gv=0.0
    grid_present=bool(gv>=GRID_PRESENT_V)
    detector_w=0.0 if (DISCHARGE_REQUIRE_GRID_ABSENT and grid_present) else discharge_w
    discharging=bool(_update_discharge_state(detector_w,now).get("discharge_detected"))

    with BMS_AUTO_LOCK:
        was=bool(BMS_AUTO_STATE.get("was_discharging"))
        if discharging:
            BMS_AUTO_STATE["was_discharging"]=True
            BMS_AUTO_STATE["post_remaining"]=0
            BMS_AUTO_STATE["last_post_request"]=0.0
            BMS_AUTO_STATE["recovered_at"]=0.0
        else:
            if was:
                BMS_AUTO_STATE["was_discharging"]=False
                BMS_AUTO_STATE["post_remaining"]=BMS_POST_RECOVERY_POLLS
                BMS_AUTO_STATE["last_post_request"]=0.0
                BMS_AUTO_STATE["recovered_at"]=now
        post_remaining=int(BMS_AUTO_STATE.get("post_remaining",0) or 0)
        last_post=float(BMS_AUTO_STATE.get("last_post_request",0) or 0)

    if discharging:
        with BMS_LOCK:
            updated=float(BMS_CACHE.get("updated",0) or 0)
            polling=bool(BMS_CACHE.get("polling"))
        age=(now-updated) if updated else 1e12
        if (not polling) and age>=max(5.0,BMS_DISCHARGE_POLL_SEC):
            bms_trigger_async()
        return

    if post_remaining>0 and (last_post<=0 or now-last_post>=max(5.0,BMS_POST_RECOVERY_POLL_SEC)):
        if bms_trigger_async():
            with BMS_AUTO_LOCK:
                if not BMS_AUTO_STATE.get("was_discharging") and BMS_AUTO_STATE.get("post_remaining",0)>0:
                    BMS_AUTO_STATE["post_remaining"]-=1
                    BMS_AUTO_STATE["last_post_request"]=now


def poll_once():
    # Core telemetry determines whether the inverter is online.
    vals200=BUS.read_chunked(200,40,20)
    status,bykey=decode_status(vals200)
    now=time.time()
    with CACHE_LOCK:
        CACHE.update(online=True,updated=now,status=status,error="")
        HISTORY.append(dict(ts=now,**{k:bykey.get(k) for k in ("grid_voltage","grid_frequency","grid_power","inverter_power","pv_voltage","pv_power","battery_voltage","net_battery_current","battery_soc","load_percent","inverter_temperature","output_active_power")}))

    maybe_trigger_bms_on_discharge(bykey)

    # Settings/RTC are optional. A temporary failure must not make status offline.
    try:
        b=read_settings_blocks()
        settings=decode_settings(b)
        with CACHE_LOCK:
            CACHE["settings"]=settings
            CACHE["settings_error"]=""
    except Exception as e:
        with CACHE_LOCK:CACHE["settings_error"]=str(e)

    try:
        rtc=read_rtc()
        with CACHE_LOCK:CACHE["rtc"]=rtc
    except Exception as e:
        with CACHE_LOCK:CACHE["rtc_error"]=str(e)

def poller():
    while True:
        t=time.time()
        try:
            poll_once()
        except Exception as e:
            now=time.time()
            with CACHE_LOCK:
                CACHE["error"]=str(e)
                # A single transient Modbus/CRC timeout must not make the UI offline
                # while recently received telemetry is still fresh.
                age=(now-CACHE.get("updated",0)) if CACHE.get("updated") else 1e9
                CACHE["online"]=age <= max(15.0,POLL_SEC*3.0)
        time.sleep(max(.1,POLL_SEC-(time.time()-t)))

def raw_items(start,vals):
    out=[]
    for i,v in enumerate(vals):
        a=start+i
        name=OBSERVED.get(a) or SETTINGS.get(a,{}).get("label") or (TELEMETRY.get(a)[1] if a in TELEMETRY else "")
        out.append(dict(register=a,raw=v,signed=s16(v),hex="0x%04X"%v,binary=f"{v:016b}",known=bool(name),name=name))
    return out

def validate(a,new_raw):
    vals={}
    for x in (324,325,332,333,341,342,343):
        try:vals[x]=BUS.read(x)[0]
        except:pass
    vals[a]=new_raw
    if vals.get(325,0)>vals.get(324,65535):raise ValueError("Float voltage cannot exceed bulk voltage")
    if vals.get(333,0)>vals.get(332,65535):raise ValueError("Utility charge current cannot exceed total max charge current")
    if 341 in vals and 342 in vals and vals[341]>=vals[342]:raise ValueError("Low-DC SOC in mains mode should be below recovery SOC")

def audit(a,old,new):
    os.makedirs(os.path.dirname(LOG_FILE),exist_ok=True)
    with open(LOG_FILE,"a",encoding="utf-8") as f:f.write("%s RS485 reg=%d old=%d new=%d verified=1\n"%(datetime.now().isoformat(timespec="seconds"),a,old,new))

HTML=r'''<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ANENJI RS485 v2.11.26</title>
<style>body{font-family:system-ui;margin:0;background:#f4f6f8;color:#181a1e}header{background:#17191d;color:white;padding:14px 20px;display:flex;justify-content:space-between;position:sticky;top:0;z-index:2}main{max-width:1220px;margin:auto;padding:18px}.card{background:white;border-radius:14px;padding:16px;margin-bottom:15px;box-shadow:0 2px 10px #0001}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(175px,1fr));gap:9px}.metric{border:1px solid #e1e5e9;border-radius:10px;padding:11px}.metric b{display:block;font-size:21px;margin-top:4px}table{width:100%;border-collapse:collapse}td,th{padding:8px;border-bottom:1px solid #eee;text-align:left;vertical-align:middle}input,select,button{font:inherit;padding:7px;border:1px solid #ccd2d8;border-radius:8px}button[disabled]{opacity:.4}.warn{background:#fff0bf;padding:10px;border-radius:9px}.tag{font-size:12px;padding:2px 6px;border-radius:8px;background:#f7dada}.known{background:#e3f4e5}.changed{background:#fff2b7}.ok{color:#55d572}.err{color:#ef7272}small{color:#666}.toolbar{display:flex;gap:9px;flex-wrap:wrap;align-items:center}.mono{font-family:ui-monospace,monospace}.group{margin-top:22px}
.flow{
  display:grid;
  grid-template-columns:minmax(180px,1fr) 72px minmax(160px,220px) 72px minmax(180px,1fr);
  grid-template-rows:auto 54px auto 54px auto;
  gap:0;
  align-items:center;
  justify-items:stretch;
  max-width:1040px;
  margin:12px auto 22px;
}
.flow-node{
  border:1px solid #d7dee5;
  border-radius:16px;
  padding:16px 18px;
  text-align:center;
  background:#fff;
  position:relative;
  z-index:2;
}
.flow-node .ico{font-size:32px;line-height:1}
.flow-node b{display:block;font-size:24px;margin-top:5px}
.flow-node small{display:block}

.flow-pv{grid-column:3;grid-row:1}
.flow-grid{grid-column:1;grid-row:3}
.flow-center{grid-column:3;grid-row:3}
.flow-load{grid-column:5;grid-row:3}
.flow-bat{grid-column:3;grid-row:5}

/* Horizontal connectors now have dedicated grid columns, so they can never
   collapse underneath a card. */
.flow-line{
  height:8px;
  width:100%;
  background:#c8d0d8;
  position:relative;
  z-index:3;
  border-radius:5px;
  align-self:center;
}
#gridLine{grid-column:2;grid-row:3}
#loadLine{grid-column:4;grid-row:3}

.flow-line.active{background:#22b45b}
.flow-line.export{background:#e53935}
.flow-line.active:after{
  content:'';
  position:absolute;
  right:-1px;
  top:50%;
  transform:translateY(-50%);
  border-top:11px solid transparent;
  border-bottom:11px solid transparent;
  border-left:18px solid #22b45b;
}
.flow-line.active.reverse:after{
  right:auto;
  left:-1px;
  border-left:0;
  border-right:18px solid #22b45b;
}
.flow-line.export:after{border-left-color:#e53935}
.flow-line.export.reverse:after{border-right-color:#e53935}

/* Vertical connectors likewise have their own rows. */
.flow-v{
  width:8px;
  height:46px;
  background:#c8d0d8;
  margin:auto;
  position:relative;
  z-index:3;
  border-radius:5px;
}
#pvDown{grid-column:3;grid-row:2}
#batVert{grid-column:3;grid-row:4}

.flow-v.active{background:#22b45b}
.flow-v.discharge{background:#e53935}
.flow-v.down.active:after{
  content:'';
  position:absolute;
  bottom:-1px;
  left:50%;
  transform:translateX(-50%);
  border-left:11px solid transparent;
  border-right:11px solid transparent;
  border-top:18px solid #22b45b;
}
.flow-v.up.active:after{
  content:'';
  position:absolute;
  top:-1px;
  left:50%;
  transform:translateX(-50%);
  border-left:11px solid transparent;
  border-right:11px solid transparent;
  border-bottom:18px solid #22b45b;
}
.flow-v.up.discharge:after{border-bottom-color:#e53935}
.flow-v.down.charge:after{border-top-color:#22b45b}

.flow-bat.discharging{border-color:#ef9a9a}
.flow-bat.charging{border-color:#8bd5a5}
.flow-bat.attention{box-shadow:0 0 0 2px #ef5350 inset}

@media(max-width:720px){
  .flow{
    grid-template-columns:minmax(108px,1fr) 38px minmax(108px,145px) 38px minmax(108px,1fr);
    grid-template-rows:auto 46px auto 46px auto;
  }
  .flow-node{padding:11px 7px}
  .flow-node b{font-size:19px}
  .flow-node .ico{font-size:27px}
  .flow-line{height:6px}
  .flow-v{width:6px;height:38px}
}
.info-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px}.info-box{border:1px solid #e1e5e9;border-radius:12px;padding:12px}.info-box h3{margin:0 0 8px}.kv{display:flex;justify-content:space-between;gap:12px;padding:4px 0;border-bottom:1px dashed #eee}.kv:last-child{border-bottom:0}@media(max-width:620px){.flow{grid-template-columns:1fr 110px 1fr;gap:8px}.flow-node{padding:10px}.flow-node b{font-size:18px}}
.pack-summary{cursor:pointer;list-style:none}.pack-summary:after{content:"▾";float:right}.pack-card[open] .pack-summary:after{content:"▴"}.pack-stale{border-color:#e2b84b;background:#fffdf5}.pack-offline{border-color:#d66;background:#fff7f7}.pack-state{font-size:12px;margin-top:4px}
.bms-card{background:#fff;border:1px solid #dfe4e8;border-radius:12px;padding:12px;cursor:pointer;transition:.15s}
.bms-card:hover{box-shadow:0 2px 10px rgba(0,0,0,.08)}
.bms-card.problem{border:2px solid #d9534f;background:#fff6f6}
.bms-card.warn{border:2px solid #e0a800;background:#fffaf0}
.bms-card.offline{border:2px solid #8a8a8a;background:#f7f7f7}
.bms-card .problem-label{font-size:12px;font-weight:700;margin-top:6px}
.bms-card .pack-status{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:5px 0 4px;font-size:12px}
.bms-card .state-pill{font-weight:700;padding:2px 7px;border-radius:999px;background:#eef2f5}
.bms-card .state-pill.ok{background:#eaf8ee;color:#238b45}
.bms-card .state-pill.warn{background:#fff4cc;color:#8a6500}
.bms-card .state-pill.err{background:#fde8e8;color:#b42318}
.delta-badge{font-size:11px;color:#68737d;padding:2px 6px;border-radius:999px;background:#f2f4f5}
.delta-badge.warn{color:#8a6500;background:#fff4cc}
.delta-badge.alarm{color:#b42318;background:#fde8e8;font-weight:700}
.modal-backdrop{position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:10000;display:flex;align-items:center;justify-content:center;padding:20px}
.modal-card{background:#fff;border-radius:14px;max-width:760px;width:min(760px,96vw);max-height:90vh;overflow:auto;padding:16px;box-shadow:0 12px 40px rgba(0,0,0,.28)}
.modal-head{display:flex;align-items:center;justify-content:space-between;gap:10px;position:sticky;top:0;background:#fff;padding-bottom:10px;border-bottom:1px solid #eee}
.modal-head h3{margin:0}
.modal-head button{font-size:20px;border:0;background:transparent;cursor:pointer}


.modal-card{font-size:13px}
.modal-card .kv{display:grid;grid-template-columns:minmax(170px,0.9fr) minmax(0,1.4fr);gap:12px;padding:5px 0;border-bottom:1px dotted #e3e3e3}
.modal-card .kv b{font-weight:700}
.modal-card .kv span{font-weight:400;text-align:left}
.cell-grid{display:grid;grid-template-columns:repeat(4,minmax(92px,1fr));gap:6px 12px;margin-top:4px}
.cell-item{display:grid;grid-template-columns:28px 1fr;gap:6px;align-items:center;padding:4px 6px;border:1px solid #ececec;border-radius:6px;background:#fafafa}
.cell-item b{font-weight:700}
.cell-item span{text-align:right;font-variant-numeric:tabular-nums}
@media(max-width:720px){.cell-grid{grid-template-columns:repeat(2,minmax(100px,1fr))}.modal-card .kv{grid-template-columns:1fr}}


#bmsAggregate .metric{padding:11px 12px;min-height:62px}
#bmsAggregate .metric small{display:block;font-size:13px;line-height:1.25;font-weight:400}
#bmsAggregate .metric b{display:block;font-size:20px;line-height:1.12;font-weight:700;margin:4px 0 5px}

</style></head>
<body><header><b>ANENJI RS485 — v2.11.26</b><span id="conn">...</span></header><main>
<div class="card"><h2>Связь / RTC</h2><div id="health"></div>
<div class="toolbar" style="margin-top:10px">
<input id="rtcLocal" type="datetime-local" step="1">
<button id="rtcWrite" onclick="setRtc()">Установить RTC</button>
<button onclick="rtcNow()">Взять время браузера</button>
</div><small>RTC: Modbus holding registers 434..439 = year, month, day, hour, minute, second. Запись выполняется одним FC16-блоком с read-back.</small></div>
<div class="card"><h2>Энергопотоки</h2>
<div class="flow">
  <div class="flow-node flow-pv"><div class="ico">☀️</div><small>PV</small><b id="fPv">— W</b><small id="fPvSub">— V · — A</small></div>
  <div id="pvDown" class="flow-v down"></div>

  <div class="flow-node flow-grid"><div class="ico">⚡</div><small>Сеть</small><b id="fGrid">— W</b><small id="fGridSub">— V · — Hz</small></div>
  <div class="flow-line" id="gridLine"></div>
  <div class="flow-node flow-center"><div class="ico">▣</div><small>Инвертор</small><b id="fInv">— W</b><small id="fInvSub">— V · — Hz</small></div>
  <div class="flow-line" id="loadLine"></div>
  <div class="flow-node flow-load"><div class="ico">🏠</div><small>Нагрузка</small><b id="fLoad">— W</b><small id="fLoadSub">— % · — VA</small></div>

  <div id="batVert" class="flow-v"></div>
  <div id="batNode" class="flow-node flow-bat"><div class="ico">🔋</div><small>Батарея</small><b id="fBat">— V</b><small id="fBatSub">— % · — A</small></div>
</div>
<div class="info-grid">
  <div class="info-box"><h3>Battery</h3><div id="infoBattery"></div></div>
  <div class="info-box"><h3>Solar / PV</h3><div id="infoPv"></div></div>
  <div class="info-box"><h3>Grid</h3><div id="infoGrid"></div></div>
  <div class="info-box"><h3>Inverter / Load</h3><div id="infoInv"></div></div>
</div></div>
<div class="card"><h2>BMS батарей <span id="bmsState"></span></h2>
<div style="display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:-4px 0 12px">
  <button id="bmsPollBtn" type="button" onclick="bmsPollNow()">Запросить состояние BMS</button>
  <small id="bmsPollMode"></small>
</div>
<div id="bmsAggregate" class="grid"></div>
<div id="bmsDevices" class="info-grid" style="margin-top:12px"></div>
<p><small>Источник: локальная saihang-bms по BLE (read-only). Опрос выполняется только по MAC-адресам из ANENJI_BMS_MACS — read-all/scan не используется. Основные напряжение/ток батарейной шины берутся с инвертора. BMS показывает модули отдельно. При кратковременном BLE timeout сохраняется последнее успешное значение; после нескольких подряд ошибок модуль помечается как недоступный.</small></p>
</div>
<div class="card"><h2>Dashboard</h2><div class="grid" id="tele"></div></div>
<div class="card"><h2>Профили батареи</h2>
<div class="toolbar">
<label>Профиль <select id="profileSelect"></select></label>
<button id="profilePreviewBtn" onclick="renderProfilePreview()">Показать</button>
<button id="profileApplyBtn" onclick="profileApply()">Применить профиль</button>
</div>
<div id="profileInfo" style="margin-top:12px"></div>
<div id="profilePreviewBox" style="margin-top:12px"></div>
<div id="profileResultWrap" style="display:block;margin-top:12px">
  <b>Результат применения</b>
  <pre id="profileResult" style="white-space:pre-wrap;user-select:text;-webkit-user-select:text;background:#f6f7f8;border:1px solid #dfe4e8;border-radius:10px;padding:12px;overflow:auto"></pre>
</div>
<p class="warn">Профиль записывается последовательно FC16, каждый регистр перечитывается и проверяется. Требуется ANENJI_ALLOW_WRITE=1. LI1..LI4 намеренно не используются: при них RS485 уходит под BMS. Регистры 323 (overvoltage), 326 и 330 профили намеренно не изменяют. F2-27 / reg 351 задаёт ограничение разрядного тока: 0=OFF, 50..500 A=лимит. В профиле Standard установлен 95 A, в Long Life — 80 A.</p>
</div>
<div class="card"><h2>Настройки</h2><p class="warn">301 Main output priority и 331 Main charging priority доступны для записи. Для 301 raw 3=SUF и raw 4=ZEC подтверждены; raw 1=SUB и raw 2=SBU пока экспериментальны. Battery type 322 разрешён для подтверждённых не-LI режимов AGM/FLD/USER. Экспериментальные регистры показаны отдельной группой и заблокированы для записи. 344 MAX feedback power to grid подтверждён как записываемый и разблокирован. 420/426 остаются заблокированы.</p>
<div class="toolbar" style="margin-bottom:10px"><button type="button" onclick="settingsLoad()">Обновить настройки</button><small id="settingsState"></small></div>
<div id="settings"></div></div>
<div class="card"><h2>Raw explorer</h2><div class="toolbar"><label>Start <input id="rstart" type="number" value="340"></label><label>Count <input id="rcount" type="number" value="60" min="1" max="80"></label><label><input id="hidezero" type="checkbox"> hide zero</label><label><input id="onlyunknown" type="checkbox"> only unknown</label><label><input id="onlychanged" type="checkbox"> only changed</label><button onclick="rawLoad()">Read</button><button onclick="snapshot()">Snapshot</button></div><div id="raw"></div></div>
</main>
<div id="bmsModal" class="modal-backdrop" style="display:none">
  <div class="modal-card">
    <div class="modal-head">
      <h3 id="bmsModalTitle">BMS details</h3>
      <button id="bmsModalClose" type="button">✕</button>
    </div>
    <div id="bmsModalBody"></div>
  </div>
</div>

<script>

const $ = id => document.getElementById(id);
const E = x => String(x ?? '').replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
let snap = {};
let profileDefs = {};

async function G(u){
  const r = await fetch(u, {cache:'no-store'});
  let j;
  try { j = await r.json(); }
  catch(e) { throw new Error(`HTTP ${r.status}: invalid JSON`); }
  if(!r.ok) throw new Error(j.error || `HTTP ${r.status} ${r.statusText}`);
  return j;
}
function val(m,k,d='—'){ const x=m[k]; return x===undefined||x===null?d:x; }
function fmt(v,d=1){ return typeof v==='number' ? v.toFixed(d) : v; }
function humanAge(sec){
  if(!Number.isFinite(sec) || sec<0) return '—';
  sec=Math.floor(sec);
  if(sec<60) return `${sec} с`;
  const m=Math.floor(sec/60);
  if(m<60) return `${m} мин ${sec%60} с`;
  const h=Math.floor(m/60);
  if(h<24) return `${h} ч ${m%60} мин`;
  const d=Math.floor(h/24);
  return `${d} д ${h%24} ч`;
}
function etaText(sec){
  if(sec===null||sec===undefined||!Number.isFinite(+sec))return '—';
  return humanAge(+sec);
}
function kv(label,value){ return `<div class="kv"><span>${E(label)}</span><b>${E(value)}</b></div>`; }



let bmsDevicesCache = [];
let bmsModalKey = localStorage.getItem('anenji_bms_modal_key') || '';

function bmsProblemLevel(d){
  if(!d || !d.pack) return 'offline';
  const c=d.cells||{},t=d.temperatures||{},s=d.status||{};
  if(!d._online || d._stale) return 'offline';
  if(s.problem) return 'problem';
  if((d._participation||{}).state==='suspect') return 'problem';
  if((c.max_v||0)>=3.50 || (c.min_v||9)<=2.90) return 'problem';
  if((t.overall_max_c||0)>=55) return 'problem';
  return '';
}

function bmsProblemText(d){
  if(!d || !d.pack) return 'No live BMS data';
  const c=d.cells||{}, t=d.temperatures||{}, s=d.status||{}, parts=[];
  if(!d._online) parts.push('offline');
  else if(d._stale) parts.push('stale');
  if(s.problem) parts.push(`BMS problem ${s.problem_code_hex||''}`.trim());
  const part=d._participation||{};
  if(part.state==='suspect') parts.push('не участвует в разряде');
  if((c.max_v||0)>=3.50) parts.push(`high cell ${fmt(c.max_v,3)} V`);
  if((c.min_v||9)<=2.90) parts.push(`low cell ${fmt(c.min_v,3)} V`);
  if((t.overall_max_c||0)>=55) parts.push(`temp ${fmt(t.overall_max_c,1)} °C`);
  return parts.join(' · ');
}
function bmsPackState(d){
  if(!d || !d.pack) return {text:'offline',cls:'err'};
  const p=d.pack||{}, s=d.status||{}, part=d._participation||{};
  if(s.problem) return {text:'BMS problem',cls:'err'};
  if(!d._online || part.state==='offline') return {text:'offline',cls:'err'};
  if(d._stale) return {text:'stale',cls:'warn'};
  if(part.state==='suspect') return {text:'не участвует в разряде',cls:'err'};
  if(part.state==='active') return {text:'разряд',cls:'ok'};
  const a=Number(p.current_a||0);
  if(a>=1 || p.charging) return {text:'заряд',cls:'ok'};
  return {text:'ожидание',cls:''};
}

function bmsDeltaBadge(d,j){
  const mv=Number((d.cells||{}).delta_mv||0);
  const warn=Number(j.cell_delta_warn_mv||80);
  const alarm=Number(j.cell_delta_alarm_mv||150);
  if(mv>=alarm) return `<span class="delta-badge alarm">Δ ${fmt(mv,0)} mV</span>`;
  if(mv>=warn) return `<span class="delta-badge warn">Δ ${fmt(mv,0)} mV</span>`;
  return `<span class="delta-badge">Δ ${fmt(mv,0)} mV</span>`;
}

function bmsDetailsHtml(d,i){
  const c=d.cells||{},t=d.temperatures||{},s=d.status||{},cap=d.capacity||{},dev=d.device||{},p=d.pack||{},pr=d.protocol||{};
  const addr=dev.address||d.address||'—';
  const lastSuccess=d._last_success||d.timestamp_unix||0;
  const cells=(c.voltages_v||[]).map((v,n)=>`<div class="cell-item"><b>${n+1}</b><span>${fmt(v,3)} V</span></div>`).join('');
  const temps=(t.cell_c||[]).map((v,n)=>`T${n+1}: ${fmt(v,1)} °C`).join(' · ');
  const row=(k,v)=>`<div class="kv"><b>${E(k)}</b><span>${v}</span></div>`;
  return `<div class="bms-detail-list">`+
    row('Address',E(addr))+
    row('Name',E(dev.name||'—'))+
    row('Voltage',fmt(p.voltage_v,3)+' V')+
    row('Current',fmt(p.current_a,2)+' A')+
    row('Power',fmt(p.power_w,0)+' W')+
    row('SOC / SOH',`${fmt(p.soc_pct,0)}% / ${fmt(p.soh_pct,0)}%`)+
    row('State',E(bmsPackState(d).text))+
    row('Charging flag',p.charging?'yes':'no')+
    row('Remaining',`${fmt(cap.remaining_ah,2)} Ah · ${fmt((cap.remaining_wh_est||0)/1000,3)} kWh`)+
    row('Design capacity',fmt(cap.design_ah,1)+' Ah')+
    row('Cycles',String(cap.cycles??'—'))+
    row('Cells min/max',`${fmt(c.min_v,3)} / ${fmt(c.max_v,3)} V`)+
    row('Min/max cell',`${c.min_cell_index??'—'} / ${c.max_cell_index??'—'}`)+
    row('Cell average',fmt(c.average_v,4)+' V')+
    row('Cell delta',fmt(c.delta_mv,1)+' mV')+
    row('Cell stddev',fmt(c.stddev_mv,1)+' mV')+
    `<div class="kv"><b>Cell voltages</b><span><div class="cell-grid">${cells}</div></span></div>`+
    row('Cell temperatures',E(temps||'—'))+
    row('MOSFET temp',fmt(t.mosfet_c,1)+' °C')+
    row('Ambient',fmt(t.ambient_c,1)+' °C')+
    row('Overall max',fmt(t.overall_max_c,1)+' °C')+
    row('Charge MOSFET',s.charge_mosfet?'ON':'OFF')+
    row('Discharge MOSFET',s.discharge_mosfet?'ON':'OFF')+
    row('Balancer',s.balancing_active?'active':'off')+
    row('Problem code',s.problem?`${E(s.problem_code_hex||'')} ${E((s.problem_bits_set||[]).join(','))}`:'OK')+
    row('CRC',pr.crc_ok===true?'OK':(pr.crc_ok===false?'BAD':'—'))+
    row('Frame length',String(pr.frame_length??'—'))+
    row('Register count',String(pr.register_count??'—'))+
    row('Consecutive failures',String(d._consecutive_failures||0))+
    row('Last successful sample',lastSuccess?E(new Date(lastSuccess*1000).toLocaleString()):'—')+
    row('Last error',E(d._error||'—'))+
    `</div>`;
}

function openBmsModal(key){
  const idx=bmsDevicesCache.findIndex((d,i)=>((d.device||{}).address||d.address||String(i))===key);
  if(idx<0)return;
  const d=bmsDevicesCache[idx], dev=d.device||{};
  const modal=$('bmsModal'), title=$('bmsModalTitle'), body=$('bmsModalBody');
  if(!modal||!title||!body)return;
  title.textContent=`Pack ${idx+1} · ${dev.name||dev.address||d.address||'BMS'}`;
  body.innerHTML=bmsDetailsHtml(d,idx);
  modal.style.display='flex';
  bmsModalKey=key;
  localStorage.setItem('anenji_bms_modal_key',key);
}
function closeBmsModal(){
  const modal=$('bmsModal');
  if(modal) modal.style.display='none';
  bmsModalKey='';
  localStorage.removeItem('anenji_bms_modal_key');
}

async function bmsPollNow(){
  const b=$('bmsPollBtn');
  if(b){b.disabled=true;b.textContent='Опрос BMS…'}
  try{
    const r=await fetch('/api/bms/poll',{method:'POST',cache:'no-store'});
    const j=await r.json();
    if(!r.ok)throw new Error(j.error||('HTTP '+r.status));
    await bmsRefresh();
  }catch(e){
    alert('BMS: '+e.message);
  }
}
window.bmsPollNow=bmsPollNow;

async function bmsRefresh(){
  try{
    const j=await G('/api/bms'),a=j.aggregate||{};
    const ageSec=j.updated?Math.max(0,(Date.now()/1000)-j.updated):Infinity;
    const ageText=humanAge(ageSec);
    const healthy=j.healthy_count??0,total=(j.devices||[]).length;
    const warnAge=+(j.warn_age_sec||3600), oldAge=+(j.old_age_sec||14400);
    bmsDevicesCache=j.devices||[];

    let bmsAgeClass='ok', bmsAgeNote='';
    if(Number.isFinite(ageSec) && ageSec>=oldAge){
      bmsAgeClass='err';
      bmsAgeNote=' · данные устарели';
    }else if(Number.isFinite(ageSec) && ageSec>=warnAge){
      bmsAgeClass='warn';
      bmsAgeNote=' · рекомендуется обновить';
    }

    const activeInfo=j.bms_expected?` · active ${j.bms_active||0}/${j.bms_expected}`:'';
    const bankWarn=j.bank_problem?` · ⚠ ${j.bank_problem_packs?.length||0} problem pack(s)`:'';
    $('bmsState').innerHTML=j.online
      ? `<span class="${j.bank_problem?'warn':bmsAgeClass}">● ${healthy}/${total} online${activeInfo} · ${ageText}${bmsAgeNote}${bankWarn}</span>`
      : `<span class="err">● offline${j.updated?' · последние данные '+ageText+' назад':''}</span>`;
    const pb=$('bmsPollBtn');
    if(pb){
      pb.disabled=!!j.polling;
      pb.textContent=j.polling?'Опрос BMS…':'Запросить состояние BMS';
    }
    const pm=$('bmsPollMode');
    if(pm){
      const base=(j.poll_sec||0)>0
        ? `Автоопрос каждые ${humanAge(j.poll_sec)}`
        : 'Только ручной опрос';
      const freshness=j.updated
        ? ` · последнее обновление ${ageText} назад`
        : ' · данных ещё нет';
      const cycle=j.cycle_duration_sec
        ? ` · цикл ${Number(j.cycle_duration_sec).toFixed(1)} с`
        : '';
      const modeInfo=` · parallel ${j.parallel||1}`+((j.retry_failed||0)>0?` · retry ${j.retry_failed}`:'');
      const autoDischarge=j.auto_on_discharge?` · auto при разряде >${fmt(j.discharge_threshold_w||0,0)} W / ${humanAge(j.discharge_poll_sec||60)}`:'';
      const recoveryInfo=(j.post_recovery_polls||0)>0
        ? ` · после восстановления ${j.post_recovery_polls}×/${humanAge(j.post_recovery_poll_sec||20)}`
        : '';
      const deltaInfo=` · Δ warn/alarm ${fmt(j.cell_delta_warn_mv||80,0)}/${fmt(j.cell_delta_alarm_mv||150,0)} mV`;
      pm.textContent=base+freshness+cycle+modeInfo+autoDischarge+recoveryInfo+deltaInfo;
    }

    if(a.count){
      $('bmsAggregate').innerHTML=
       `<div class="metric"><small>BMS modules</small><b>${a.count}</b><small>healthy ${healthy}/${total}</small></div>`+
       `<div class="metric"><small>Pack voltages</small><b>${fmt(a.pack_voltage_min_v,2)}–${fmt(a.pack_voltage_max_v,2)} V</b><small>spread ${fmt(a.pack_voltage_spread_v,3)} V</small></div>`+
       `<div class="metric"><small>Pack currents</small><b>${fmt(a.pack_current_min_a,1)}…${fmt(a.pack_current_max_a,1)} A</b><small>avg ${fmt(a.pack_current_avg_a,1)} A</small></div>`+
       `<div class="metric"><small>SOC</small><b>${fmt(a.soc_min_pct,0)}–${fmt(a.soc_max_pct,0)} %</b><small>avg ${fmt(a.soc_avg_pct,1)}% · spread ${fmt(a.soc_spread_pct,1)}%</small></div>`+
       `<div class="metric"><small>Temperature</small><b>${fmt(a.temperature_max_c,1)} °C</b><small>${a.balancing_active?'balancing':'not balancing'}</small></div>`+
       `<div class="metric"><small>Reported remaining energy</small><b>${fmt((a.remaining_wh_est||0)/1000,2)} kWh</b><small>sum of module BMS estimates · SOH min ${fmt(a.soh_min_pct,0)}%</small></div>`;
    }else{
      $('bmsAggregate').innerHTML=`<span class="err">${E(j.error||'No BMS data')}</span>`;
    }

    $('bmsDevices').innerHTML=bmsDevicesCache.map((d,i)=>{
      const dev=d.device||{}, addr=dev.address||d.address||'—';
      if(!d.pack){
        const key=addr||String(i);
        return `<div class="bms-card offline" onclick="openBmsModal('${E(key)}')"><b>Pack ${i+1}</b><div class="problem-label err">${E(d.error||d._error||'offline')}</div></div>`;
      }
      const c=d.cells||{},t=d.temperatures||{},p=d.pack||{},cap=d.capacity||{};
      const level=bmsProblemLevel(d);
      const key=addr||String(i);
      const problem=bmsProblemText(d);
      const st=bmsPackState(d);
      return `<div class="bms-card ${level}" onclick="openBmsModal('${E(key)}')">`+
        `<b>Pack ${i+1} · ${E(dev.name||addr)}</b>`+
        `<div class="pack-status"><span class="state-pill ${st.cls}">${E(st.text)}</span>${bmsDeltaBadge(d,j)}</div>`+
        kv('Voltage',fmt(p.voltage_v,3)+' V')+
        kv('Current',fmt(p.current_a,2)+' A')+
        kv('Power',fmt(p.power_w,0)+' W')+
        kv('SOC',fmt(p.soc_pct,0)+' %')+
        kv('Remaining',`${fmt(cap.remaining_ah,1)} Ah · ${fmt((cap.remaining_wh_est||0)/1000,2)} kWh`)+
        kv('Temp',fmt(t.overall_max_c,1)+' °C')+
        kv('Cycles',String(cap.cycles??'—'))+
        (problem?`<div class="problem-label err">⚠ ${E(problem)}</div>`:'')+
        `</div>`;
    }).join('');

    // Keep an already opened modal open across refreshes and refresh its content.
    if(bmsModalKey){
      const exists=bmsDevicesCache.some((d,i)=>((d.device||{}).address||d.address||String(i))===bmsModalKey);
      if(exists) openBmsModal(bmsModalKey);
    }
  }catch(e){
    $('bmsState').innerHTML='<span class="err">● '+E(e.message)+'</span>';
  }
}

async function refresh(){
  try{
    const j=await G('/api/cache');
    const ageN=j.updated ? ((Date.now()/1000)-j.updated) : 1e9;
    const age=j.updated ? ageN.toFixed(1) : 'never';
    const fresh=ageN <= +(j.fresh_sec||Math.max(15,(j.poll_sec||5)*3));
    $('conn').innerHTML=fresh
      ? '<span class="ok">● RS485 online</span>'
      : '<span class="err">● RS485 offline</span>';
    $('health').innerHTML=
      `<b>${E(j.port)}</b> · ${j.baud} 8N1 · slave ${j.slave} · cache ${age}s · `+
      `TX ${j.bus.tx} · RX ${j.bus.rx} · errors ${j.bus.errors} · timeouts ${j.bus.timeouts}`+
      `<br><b>RTC:</b> ${E(j.rtc||'unknown')}`+
      `<br><small>${j.error?('last poll: '+E(j.error)):''} ${j.settings_error?' · settings: '+E(j.settings_error):''} ${j.rtc_error?' · rtc: '+E(j.rtc_error):''}</small>`;

    const status=Array.isArray(j.status)?j.status:[];
    $('tele').innerHTML=status.map(x=>
      `<div class="metric"><small>${E(x.label)} · ${x.register}</small>`+
      `<b>${E(x.value)} ${E(x.unit||'')}</b><small>raw ${x.raw}</small></div>`
    ).join('');

    const m={}; status.forEach(x=>m[x.key]=x.value);
    const pv=+val(m,'pv_power',0), gp=+val(m,'grid_power',0),
          ip=+val(m,'inverter_power',0), load=+val(m,'output_active_power',0),
          bc=+val(m,'net_battery_current',val(m,'battery_current',0)),
          gv=+val(m,'grid_voltage',0),
          gridA=gv>1?Math.abs(gp)/gv:0;

    $('fPv').textContent=`${fmt(pv,0)} W`;
    $('fPvSub').textContent=`${fmt(val(m,'pv_voltage',0),1)} V · ${fmt(val(m,'pv_current',0),1)} A`;
    $('fGrid').textContent=`${fmt(gp,0)} W`;
    $('fGridSub').textContent=`${fmt(gv,1)} V · ≈${fmt(gridA,1)} A · ${fmt(val(m,'grid_frequency',0),2)} Hz`;
    $('fInv').textContent=`${fmt(ip,0)} W`;
    $('fInvSub').textContent=`${fmt(val(m,'inverter_voltage',0),1)} V · ${fmt(val(m,'inverter_current',0),1)} A · ${fmt(val(m,'inverter_frequency',0),2)} Hz`;
    $('fLoad').textContent=`${fmt(load,0)} W`;
    $('fLoadSub').textContent=`${fmt(val(m,'load_percent',0),0)} % · ${fmt(val(m,'output_current',0),1)} A · ${fmt(val(m,'output_apparent_power',0),0)} VA`;
    $('fBat').textContent=`${fmt(val(m,'battery_voltage',0),1)} V`;
    const en=j.energy||{};
    let etaPart='';
    if(en.state==='discharging'&&en.eta_to_reserve_sec!=null) etaPart=` · ETA ${etaText(en.eta_to_reserve_sec)} до ${fmt(en.reserve_soc_pct,0)}%`;
    else if(en.state==='charging'&&en.eta_to_full_sec!=null) etaPart=` · full ~${etaText(en.eta_to_full_sec)}`;
    const activePart=en.bms_total?` · active ${en.bms_active||0}/${en.bms_total}`:'';
    const problemPart=en.battery_problem?` · ⚠ ${en.battery_problem_level||'warning'}`:'';
    const attentionMode=en.attention_require_grid_absent?'no-grid':'any discharge';
    const attentionPart=en.attention_required
      ? ` · ⚠ ATTENTION (${attentionMode})`
      : (en.attention_pending?` · alert in ${etaText(Math.max(0,(en.attention_on_delay_sec||0)-(en.attention_pending_sec||0)))} (${attentionMode})`:'');
    const dischargeModePart=en.discharge_require_grid_absent?' · discharge: no-grid only':'';
    $('fBatSub').textContent=`${fmt(val(m,'battery_soc',0),0)} % · ${fmt(bc,1)} A${activePart}${etaPart}${problemPart}${attentionPart}${dischargeModePart}`;

    $('pvDown').className='flow-v down '+(pv>5?'active':'');

    // Grid: positive = import (grid -> inverter), negative = export (inverter -> grid).
    if(gp>5) $('gridLine').className='flow-line active';
    else if(gp<-5) $('gridLine').className='flow-line active reverse export';
    else $('gridLine').className='flow-line';

    // Load is always an outgoing flow from inverter when active.
    $('loadLine').className='flow-line '+((load>5||ip>5)?'active':'');

    // Unified discharge detector: independent of mains presence.
    if(en.discharge_detected) $('batVert').className='flow-v up active discharge';
    else if(bc>.2) $('batVert').className='flow-v down active charge';
    else $('batVert').className='flow-v';

    const batNode=$('batNode');
    if(batNode){
      batNode.className='flow-node flow-bat'+
        (en.discharge_detected?' discharging':(bc>.2?' charging':''))+
        (en.attention_required?' attention':'');
    }

    $('infoBattery').innerHTML=
      kv('Voltage',fmt(val(m,'battery_voltage',0),1)+' V')+
      kv('Current',fmt(bc,1)+' A')+
      kv('SOC',fmt(val(m,'battery_soc',0),0)+' %')+
      kv('Active remaining',en.active_remaining_wh?fmt(en.active_remaining_wh/1000,2)+' kWh':'—')+
      kv('Active usable to reserve',en.active_usable_to_reserve_wh?fmt(en.active_usable_to_reserve_wh/1000,2)+' kWh':'—')+
      kv('10-min battery avg',en.battery_avg_power_w!=null?fmt(en.battery_avg_power_w,0)+' W':'—')+
      kv('Estimated autonomy',en.eta_to_reserve_sec!=null?etaText(en.eta_to_reserve_sec):'—')+
      kv('Charge from inverter',fmt(val(m,'inverter_charge_current',0),1)+' A')+
      kv('Charge from PV',fmt(val(m,'pv_charge_current',0),1)+' A');

    $('infoPv').innerHTML=
      kv('Voltage',fmt(val(m,'pv_voltage',0),1)+' V')+
      kv('Current',fmt(val(m,'pv_current',0),1)+' A')+
      kv('Power',fmt(pv,0)+' W')+
      kv('Charging power',fmt(val(m,'pv_charging_power',0),0)+' W');

    $('infoGrid').innerHTML=
      kv('Voltage',fmt(gv,1)+' V')+
      kv('Current (estimated P/V)','≈'+fmt(gridA,2)+' A')+
      kv('Frequency',fmt(val(m,'grid_frequency',0),2)+' Hz')+
      kv('Power',fmt(gp,0)+' W')+
      kv('Direction',gp< -5?'Export to grid':(gp>5?'Import from grid':'Idle'));

    $('infoInv').innerHTML=
      kv('Inverter voltage',fmt(val(m,'inverter_voltage',0),1)+' V')+
      kv('Inverter current',fmt(val(m,'inverter_current',0),1)+' A')+
      kv('Frequency',fmt(val(m,'inverter_frequency',0),2)+' Hz')+
      kv('Inverter power',fmt(ip,0)+' W')+
      kv('Output current',fmt(val(m,'output_current',0),1)+' A')+
      kv('Load active',fmt(load,0)+' W')+
      kv('Load apparent',fmt(val(m,'output_apparent_power',0),0)+' VA')+
      kv('Load',fmt(val(m,'load_percent',0),0)+' %')+
      kv('DCDC temp',fmt(val(m,'dcdc_temperature',0),0)+' °C')+
      kv('Inverter temp',fmt(val(m,'inverter_temperature',0),0)+' °C');
  }catch(e){
    $('conn').innerHTML='<span class="err">● '+E(e.message)+'</span>';
  }
}

function rtcNow(){
  const d=new Date(), p=n=>String(n).padStart(2,'0');
  $('rtcLocal').value=`${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
async function setRtc(){
  if(!$('rtcLocal').value){ rtcNow(); return; }
  const d=new Date($('rtcLocal').value);
  if(Number.isNaN(d.getTime())){ alert('Некорректная дата/время'); return; }
  const body={year:d.getFullYear(),month:d.getMonth()+1,day:d.getDate(),hour:d.getHours(),minute:d.getMinutes(),second:d.getSeconds()};
  if(!confirm(`Установить RTC инвертора: ${body.year}-${String(body.month).padStart(2,'0')}-${String(body.day).padStart(2,'0')} ${String(body.hour).padStart(2,'0')}:${String(body.minute).padStart(2,'0')}:${String(body.second).padStart(2,'0')}?`)) return;
  const r=await fetch('/api/time/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const j=await r.json();
  if(!r.ok){ alert(j.error||r.statusText); return; }
  alert('RTC записан и проверен: '+j.rtc); refresh();
}

function profileFmt(reg,raw){
  const scales={323:10,324:10,325:10,326:10,327:10,329:10,332:10,333:10};
  const units={323:'V',324:'V',325:'V',326:'V',327:'V',329:'V',330:'min',332:'A',333:'A',341:'%',342:'%',343:'%'};
  if(reg===313) return raw===0?'Disabled':'Enabled';
  if(reg===322) return raw===2?'USER / USE':String(raw);
  const v=scales[reg]?raw/scales[reg]:raw;
  return `${v} ${units[reg]||'raw'}`;
}
async function profilesLoad(){
  try{
    const j=await G('/api/profiles');
    profileDefs=j.profiles||{};
    $('profileSelect').innerHTML=Object.entries(profileDefs)
      .map(([id,p])=>`<option value="${E(id)}">${E(p.label)}</option>`).join('');
    renderProfilePreview();
    $('profileApplyBtn').disabled=!j.write_enabled;
    if(!j.write_enabled) $('profileResult').textContent='Запись отключена. Запустите с ANENJI_ALLOW_WRITE=1.';
  }catch(e){
    $('profileInfo').innerHTML='<span class="err">'+E(e.message)+'</span>';
  }
}
function renderProfilePreview(){
  const id=$('profileSelect').value, p=profileDefs[id];
  if(!p){ $('profilePreviewBox').innerHTML='<small>Нет профилей</small>'; return; }
  $('profileInfo').innerHTML=
    `<b>${E(p.label)}</b><br><small>${E(p.description)}</small>`+
    `<br><small>${E(p.chemistry)} · ${p.nominal_voltage} V · ${p.capacity_ah} Ah · ${p.energy_kwh} kWh · charge limit ${p.charge_limit_a} A</small>`;
  const labels={313:'Equalization',322:'Battery type',324:'Bulk/CV',325:'Float',327:'Low DC mains',329:'Low DC off-grid',332:'Max charging current',333:'Max utility charging current',341:'Low DC SOC mains',342:'Recovery SOC',343:'Off-grid cut-off SOC',351:'Max battery discharge current'};
  let h='<table><thead><tr><th>Reg</th><th>Параметр</th><th>Значение</th><th>Raw</th></tr></thead><tbody>';
  for(const [r,v] of Object.entries(p.registers||{})){
    h+=`<tr><td>${r}</td><td>${E(labels[r]||'')}</td><td>${E(profileFmt(+r,v))}</td><td>${v}</td></tr>`;
  }
  $('profilePreviewBox').innerHTML=h+'</tbody></table>';
}
async function profileApply(){
  const id=$('profileSelect').value, p=profileDefs[id];
  if(!p) return;
  if(!confirm(`Применить профиль "${p.label}"?\nБудут последовательно записаны ${Object.keys(p.registers||{}).length} регистров с read-back.`)) return;

  $('profileApplyBtn').disabled=true;
  $('profileResultWrap').style.display='block';
  $('profileResult').textContent='Выполняется...';

  try{
    const r=await fetch('/api/profile/apply',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({profile:id})});
    let j={};
    try{ j=await r.json(); }catch(e){}
    const rows=(j.results||[]).map(x=>{
      if(x.ok && x.skipped) return `OK   reg ${x.register}: ${x.old_raw} (без изменения)`;
      if(x.ok) return `OK   reg ${x.register}: ${x.old_raw} -> ${x.new_raw}`;
      return `ERR  reg ${x.register}: ${x.error}`;
    });
    const header=r.ok ? `Профиль применён: ${p.label}` : `Профиль остановлен: ${j.error||`HTTP ${r.status} ${r.statusText}`}`;
    $('profileResult').textContent=header+'\n\n'+rows.join('\n')+(!r.ok?`\n\nHTTP ${r.status}`:'');
    if(r.ok){ await settingsLoad(); await refresh(); }
  }catch(e){
    $('profileResult').textContent='Ошибка запроса: '+e.message;
  }finally{
    // /api/profiles tells us if writes are allowed; don't blindly enable.
    try{
      const j=await G('/api/profiles');
      $('profileApplyBtn').disabled=!j.write_enabled;
    }catch(e){
      $('profileApplyBtn').disabled=false;
    }
  }
}

async function settingsLoad(){
  try{
    const j=await G('/api/settings'), groups={};
    const rows=Array.isArray(j.data)?j.data:[];
    rows.forEach(x=>(groups[x.group]??=[]).push(x));

    const st=$('settingsState');
    if(st){
      const ok=rows.filter(x=>x.read_ok!==false).length;
      st.innerHTML=`Прочитано ${ok}/${rows.length}`+(j.error?` · <span class="warn">${E(j.error)}</span>`:'');
    }

    let h='';
    for(const [g,arr] of Object.entries(groups)){
      h+=`<div class="group"><h3>${E(g)}</h3><table><thead><tr><th>Reg</th><th>Параметр</th><th>Сейчас</th><th>Новое</th><th></th></tr></thead><tbody>`;
      for(const x of arr){
        const readable=x.read_ok!==false && x.raw!==null && x.raw!==undefined;
        let ctl='—';

        if(readable){
          ctl=x.kind==='select'
            ? `<select id="v${x.register}">${Object.entries(x.values||{}).map(([v,n])=>`<option value="${v}" ${+v===x.raw?'selected':''}>${E(n)}</option>`).join('')}</select>`
            : x.kind==='action'
              ? '—'
              : `<input id="v${x.register}" type="number" value="${x.value}" min="${x.min}" max="${x.max}" step="${x.step}">`;
        }

        const current=readable
          ? `${E(x.register===351&&x.raw===0?'OFF':x.display)}<br><small>raw ${x.raw}</small>`
          : `<span class="warn">Unavailable</span>`;

        h+=`<tr><td>${x.register}</td>`+
           `<td>${E(x.label)} ${x.safe?'':'<span class="tag">locked</span>'}<br><small>${E(x.note||'')}</small></td>`+
           `<td>${current}</td><td>${ctl}</td>`+
           `<td><button ${x.write_enabled&&readable?'':'disabled'} onclick="W(${x.register},'${readable?E(x.display):''}')">Записать</button></td></tr>`;
      }
      h+='</tbody></table></div>';
    }

    $('settings').innerHTML=h || '<span class="warn">Настройки пока не прочитаны. Нажмите «Обновить настройки».</span>';
  }catch(e){
    $('settings').innerHTML='<span class="err">'+E(e.message)+'</span>';
    const st=$('settingsState'); if(st)st.textContent='Ошибка чтения';
  }
}

async function W(reg,oldv){
  const el=$('v'+reg);
  if(!el){alert('Сначала необходимо успешно прочитать этот регистр');return;}
  const v=+el.value;
  if(!confirm(`RS485 FC16\nreg ${reg}\n${oldv} → ${v}\nЗаписать?`))return;
  const r=await fetch('/api/write',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({register:reg,value:v})});
  const j=await r.json();
  if(!r.ok){alert(j.error||r.statusText);return}
  alert('Записано и подтверждено');
  settingsLoad();refresh();
}
async function rawLoad(){
  try{
    const s=+$('rstart').value,c=+$('rcount').value,j=await G(`/api/raw?start=${s}&count=${c}`);
    let h='<table><thead><tr><th>Reg</th><th>Name</th><th>Raw</th><th>Signed</th><th>Hex</th><th>Binary</th><th>Δ</th></tr></thead><tbody>';
    for(const x of j.data||[]){
      const ch=snap[x.register]!==undefined&&snap[x.register]!==x.raw;
      if($('hidezero').checked&&x.raw===0)continue;
      if($('onlyunknown').checked&&x.name)continue;
      if($('onlychanged').checked&&!ch)continue;
      const d=snap[x.register]===undefined?'':x.raw-snap[x.register];
      h+=`<tr class="${x.name?'known ':''}${ch?'changed':''}"><td>${x.register}</td><td>${E(x.name||'')}</td><td>${x.raw}</td><td>${x.signed}</td><td class="mono">${x.hex}</td><td class="mono">${x.binary}</td><td>${d}</td></tr>`;
    }
    $('raw').innerHTML=h+'</tbody></table>';
  }catch(e){
    $('raw').innerHTML='<span class="err">'+E(e.message)+'</span>';
  }
}
async function snapshot(){
  const j=await G(`/api/raw?start=${+$('rstart').value}&count=${+$('rcount').value}`);
  snap={};(j.data||[]).forEach(x=>snap[x.register]=x.raw);
  try{localStorage.setItem('anenji294snap',JSON.stringify(snap))}catch(e){}
  rawLoad();
}

window.setRtc=setRtc;
window.rtcNow=rtcNow;
window.renderProfilePreview=renderProfilePreview;
window.profileApply=profileApply;
window.W=W;
window.rawLoad=rawLoad;
window.snapshot=snapshot;

try{snap=JSON.parse(localStorage.getItem('anenji294snap')||'{}')}catch(e){snap={}}
$('profilePreviewBtn').onclick=renderProfilePreview;
$('profileSelect').onchange=renderProfilePreview;
const _bmsModal=$('bmsModal'), _bmsModalClose=$('bmsModalClose');
if(_bmsModalClose) _bmsModalClose.onclick=closeBmsModal;
if(_bmsModal) _bmsModal.addEventListener('click',e=>{if(e.target===_bmsModal)closeBmsModal()});
window.openBmsModal=openBmsModal;
window.closeBmsModal=closeBmsModal;
rtcNow();
refresh();
bmsRefresh();
profilesLoad();
settingsLoad();
rawLoad();
setInterval(refresh,2000);
setInterval(bmsRefresh,5000);
setInterval(settingsLoad,30000);

</script>
</body></html>'''

class H(BaseHTTPRequestHandler):
    def log_message(self,fmt,*args):print("%s - %s"%(self.address_string(),fmt%args))
    def J(self,o,code=200):
        d=json.dumps(o,ensure_ascii=False).encode()
        try:self.send_response(code);self.send_header("Content-Type","application/json; charset=utf-8");self.send_header("Cache-Control","no-store");self.send_header("Content-Length",str(len(d)));self.end_headers();self.wfile.write(d)
        except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):pass
    def RGB(self,o,code=200):
        # Compact endpoint for small MCU displays.
        # Short keys intentionally reduce UART/HTTP transfer time:
        # p=PV W, l=load W, g=grid W, b=battery W,
        # v=battery V, c=SOC %, d=discharging, a=attention_required,
        # e=ETA to reserve seconds (-1 if unavailable), t=local HH:MM,
        # s=battery state: -1 charge, 0 idle, 1 discharge.
        d=json.dumps(o,ensure_ascii=True,separators=(",",":")).encode("ascii")
        try:self.send_response(code);self.send_header("Content-Type","application/json");self.send_header("Cache-Control","no-store");self.send_header("Content-Length",str(len(d)));self.end_headers();self.wfile.write(d)
        except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):pass
    def do_GET(self):
        p=urlparse(self.path)
        if p.path=="/":
            d=HTML.encode()
            try:self.send_response(200);self.send_header("Content-Type","text/html; charset=utf-8");self.send_header("Content-Length",str(len(d)));self.end_headers();self.wfile.write(d)
            except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):pass
            return
        try:
            if p.path=="/api/bms":
                with BMS_LOCK:b=json.loads(json.dumps(BMS_CACHE))
                es=energy_summary()
                part={str(x.get("mac","")).upper():x for x in es.get("pack_participation",[])}
                for d in b.get("devices",[]):
                    addr=str((d.get("device") or {}).get("address") or d.get("address") or "").upper()
                    if addr in part:
                        d["_participation"]=part[addr]
                return self.J({"ok":True,**b,
                    "auto_on_discharge":BMS_AUTO_ON_DISCHARGE,
                    "discharge_threshold_w":BMS_DISCHARGE_THRESHOLD_W,
                    "discharge_poll_sec":BMS_DISCHARGE_POLL_SEC,
                    "cell_delta_warn_mv":BMS_CELL_DELTA_WARN_MV,
                    "cell_delta_alarm_mv":BMS_CELL_DELTA_ALARM_MV,
                    "post_recovery_polls":BMS_POST_RECOVERY_POLLS,
                    "post_recovery_poll_sec":BMS_POST_RECOVERY_POLL_SEC,
                    "post_recovery_remaining":BMS_AUTO_STATE.get("post_remaining",0),
                    "bank_problem":es.get("battery_problem",False),
                    "bank_problem_level":es.get("battery_problem_level","none"),
                    "bank_problem_codes":es.get("battery_problem_codes",[]),
                    "bank_problem_packs":es.get("battery_problem_packs",[]),
                    "bms_active":es.get("bms_active",0),
                    "bms_expected":es.get("bms_expected",len(BMS_MACS))})
            if p.path=="/api/cache":
                with CACHE_LOCK:c=dict(CACHE)
                c.update(port=PORT,baud=BAUD,slave=SLAVE,poll_sec=POLL_SEC,fresh_sec=max(15.0,POLL_SEC*3.0),write_enabled=ALLOW_WRITE,bus=dict(tx=BUS.tx,rx=BUS.rx,errors=BUS.errors,timeouts=BUS.timeouts,last_error=BUS.last_error,last_ok=BUS.last_ok),energy=energy_summary());return self.J(c)
            if p.path=="/api/rgb":
                es=energy_summary()
                batt_w=float(es.get("battery_power_w") or 0.0)
                dis=bool(es.get("battery_discharging"))
                # Explicit battery state for the panel:
                #  1 = discharge (authoritative discharge detector)
                # -1 = charging (negative battery power above a small deadband)
                #  0 = idle
                bat_state=1 if dis else (-1 if batt_w<=-RGB_CHARGE_DETECT_W else 0)
                eta=es.get("eta_to_reserve_sec")
                # For the panel use inverter register battery_soc.
                # It is polled together with the main inverter telemetry and
                # therefore changes immediately with the energy dashboard.
                _soc=es.get("battery_soc_pct")
                try:
                    _soc=float(_soc)
                    soc=int(round(_soc)) if 0.0 <= _soc <= 100.0 else -1
                except Exception:
                    soc=-1
                return self.RGB({
                    "p":int(round(float(es.get("pv_w") or 0.0))),
                    "l":int(round(float(es.get("load_w") or 0.0))),
                    "g":int(round(float(es.get("grid_w") or 0.0))),
                    "r":1 if bool(es.get("grid_present")) else 0,
                    "b":int(round(batt_w)),
                    "v":round(float(es.get("battery_voltage_v") or 0.0),1),
                    "c":soc,
                    "d":1 if dis else 0,
                    "a":1 if bool(es.get("attention_required")) else 0,
                    "e":int(round(float(eta))) if eta is not None else -1,
                    "t":time.strftime("%H:%M"),
                    "s":bat_state
                })
            if p.path=="/api/summary":
                return self.J({"ok":True,**energy_summary()})
            if p.path=="/api/status":
                with CACHE_LOCK:return self.J({"ok":True,"data":list(CACHE["status"]),"updated":CACHE["updated"],"online":CACHE["online"]})
            if p.path=="/api/settings":
                with CACHE_LOCK:
                    cached=list(CACHE["settings"])
                    updated=CACHE["updated"]
                    serr=CACHE.get("settings_error","")
                if not cached:
                    try:
                        with BUS.lock:
                            b=read_settings_blocks()
                        cached=decode_settings(b)
                        with CACHE_LOCK:
                            CACHE["settings"]=cached
                            CACHE["settings_error"]=""
                    except Exception as e:
                        serr=str(e)
                        with CACHE_LOCK:CACHE["settings_error"]=serr
                return self.J({"ok":True,"data":cached,"updated":updated,"write_enabled":ALLOW_WRITE,"error":serr})
            if p.path=="/api/history":return self.J({"ok":True,"data":list(HISTORY)})
            if p.path=="/api/profiles":
                public={}
                for pid,pf in BATTERY_PROFILES.items():
                    public[pid]=dict(pf)
                    public[pid]["registers"]={str(k):v for k,v in pf["registers"].items()}
                return self.J({"ok":True,"profiles":public,"write_enabled":ALLOW_WRITE})
            if p.path=="/api/confirmed-map":
                return self.J({"ok":True,"transport":"RS485","port":PORT,"baud":BAUD,"slave":SLAVE,
                  "confirmed":{
                    "201":"Operating mode",
                    "202":"Grid voltage /10 V","203":"Grid frequency /100 Hz","204":"Grid power signed W",
                    "205":"Inverter voltage /10 V","206":"Inverter current /10 A","207":"Inverter frequency /100 Hz","208":"Inverter power W",
                    "219":"PV voltage /10 V","220":"PV current /10 A","223":"PV power W",
                    "226":"DCDC temperature C","227":"Inverter temperature C","229":"Battery SOC %",
                    "300":"AC output mode","301":{"name":"Main output priority","enum":{"1":"SUB (experimental)","2":"SBU (experimental)","3":"SUF (confirmed)","4":"ZEC (confirmed)"}},
                    "302":"Input voltage range","303":"Buzzer","305":"Backlight","306":"LCD auto return",
                    "307":"Power saving","308":"Overload restart","309":"Over-temperature restart","310":"Overload bypass",
                    "320":"Output voltage","321":"Output frequency","322":{"name":"Battery type","enum":{"0":"AGM (confirmed)","1":"FLD (confirmed)","2":"USER/USE (confirmed)"}, "note":"LI1..LI4 intentionally not exposed for write because tested LI modes reserve RS485 for BMS communication."},
                    "324":"Bulk charging voltage /10 V","325":"Float charging voltage /10 V",
                    "326":"Recovery voltage back to mains /10 V","327":"Low DC protection voltage in mains /10 V",
                    "329":"Low DC protection voltage off-grid /10 V","330":"CV to float time",
                    "331":{"name":"Main charging priority","enum":{"1":"SOF","2":"SNU","3":"OSO","4":"SOR"}},
                    "332":"Maximum charging current /10 A","333":"Maximum utility charging current /10 A",
                    "334":"Equalization voltage /10 V","335":"Equalization time min","336":"Equalization timeout min","337":"Equalization interval day",
                    "341":"Low DC protection SOC in mains %","342":"Recovery SOC back to mains %","343":"Low DC cut-off SOC off-grid %",
                    "344":"MAX feedback power to grid W",
                    "420":"Output control (0=OFF observed)",
                    "434":"RTC year","435":"RTC month","436":"RTC day","437":"RTC hour","438":"RTC minute","439":"RTC second"
                  },
                  "experimental":{
                    "340":"Unknown enum; raw 3 repeatedly observed",
                    "349":"Unknown; raw 100 repeatedly observed",
                    "355":"Unknown; raw 90 observed",
                    "356":"Unknown; raw 6 observed",
                    "357":"Unknown; raw 100 observed",
                    "359":"Possible shadow/duplicate of reg 324 Bulk voltage; NOT confirmed",
                    "360":"Possible shadow/duplicate of reg 325 Float voltage; NOT confirmed",
                    "361":"Possible shadow/duplicate of reg 332 Max charging current; NOT confirmed",
                    "402":"Possible shadow/state of reg 301 Main output priority; NOT confirmed",
                    "403":"Possible shadow/state of reg 331 Main charging priority; NOT confirmed",
                    "404":"Unknown enum/state; raw 3 observed",
                    "421":"Unknown; raw 120 observed",
                    "423":"Unknown flag/state; raw 1 observed"
                  },
                  "battery_profiles":{
                    "2x_ANJ_STANDARD":"2× ANJ-25.6V-100AH-S, 51.2V 100Ah 5.12kWh, USER, Bulk 56.4V, Float 54.0V, charge limit 10A, discharge limit 95A",
                    "2x_ANJ_LONG_LIFE":"2× ANJ-25.6V-100AH-S, 51.2V 100Ah 5.12kWh, USER, Bulk 56.0V, Float 54.0V, charge limit 10A, discharge limit 80A"
                  },
                  "notes":[
                    "All tested LI battery modes disable normal RS485 Modbus access and switch/reserve the port for lithium BMS communication; only AGM/FLD/USER are exposed for write in this UI.",
                    "Slave charging priority OFF/SOF did not change master slave-id 1 registers 340..419.",
                    "301 Main output priority: raw 3=SUF and raw 4=ZEC confirmed experimentally; raw 1=SUB and raw 2=SBU remain experimental until verified on-device.",
                    "402 is an unconfirmed shadow/state candidate for 301; 403 is an unconfirmed shadow/state candidate for 331; 404 remains unknown.",
                    "Experimental observed registers: 340=3, 349=100, 355=90, 356=6, 357=100, 359=540, 360=540, 361 variable, 402=3, 403=3, 404=3, 421=120, 423=1.",
                    "Slave charging priority OFF/SOF produced no change in slave-id 1 registers 340..419; do not map it to 402..404."
                  ]})
            if p.path=="/api/manual-map":
                return self.J({"ok":True,"note":"LCD program numbers are not assumed to equal Modbus addresses.",
                  "confirmed_from_manual":{
                    "F0":["01 AC input voltage range","02 Power saving mode","03 Overload bypass","04 Auto restart on overload","05 Auto restart on over-temperature","06 Auto bypass","07 Auto return default display","08 Backlight","09 Buzzer","10 Modbus ID","12 External CT","13 CT error compensation"],
                    "F1":["01 Output source priority","02 AC output mode","03 Output voltage","04 Output frequency","05 Max feedback power to grid","06 Slave output source priority","07-10 Slave priority timer"],
                    "F2":["01 Battery type","02 Charger source priority","03 Bulk charging voltage","04 Floating charging voltage","05 Back-to-grid voltage","06 Back-to-battery voltage","07 Low DC cut-off voltage","08 Maximum charging current","09 Maximum utility charging current","15 Bulk charging time","16 Battery equalization","17 Equalization voltage","18 Equalization time","19 Equalization timeout","20 Equalization interval","21 Equalization immediately","22 Wake dormant lithium battery","23 Automatic activation","24 Max charging current","25 SOC high lock-to-battery","26 Low DC cut-off SOC","27 Max battery discharge current"],
                    "F3":["01 Year","02 Month","03 Day","04 Hour","05 Minute","06 Second"],
                    "F4":["01 Reset energy counters","02 Reset data log"]}})
            if p.path=="/api/raw":
                q=parse_qs(p.query);start=int(q.get("start",["340"])[0]);count=int(q.get("count",["60"])[0])
                if count<1 or count>80:raise ValueError("count must be 1..80")
                return self.J({"ok":True,"start":start,"count":count,"data":raw_items(start,BUS.read_chunked(start,count,20))})
            return self.J({"error":"Not found"},404)
        except Exception as e:return self.J({"error":str(e)},500)
    def do_POST(self):
        path=urlparse(self.path).path
        if path=="/api/bms/poll":
            if not BMS_ENABLE:return self.J({"ok":False,"error":"BMS disabled"},409)
            started=bms_trigger_async()
            with BMS_LOCK:
                state={"polling":BMS_CACHE.get("polling",False),"last_started":BMS_CACHE.get("last_started",0)}
            return self.J({"ok":True,"started":started,**state},202 if started else 200)
        if path=="/api/profile/apply":
            try:
                if not ALLOW_WRITE: raise PermissionError("Global writes disabled")
                n=int(self.headers.get("Content-Length","0")); b=json.loads(self.rfile.read(n) or b"{}")
                pid=str(b.get("profile",""))
                if pid not in BATTERY_PROFILES: raise ValueError("Unknown battery profile")
                pf=BATTERY_PROFILES[pid]

                # Deliberate safe order: disable equalization, select USER,
                # set charge-current limits, then voltages/SOC thresholds.
                order=[313,322,332,333,351,323,324,325,326,327,329,330,341,342,343]
                target_order=[a for a in order if a in pf["registers"]]
                results=[]
                with BUS.lock:
                    for a in target_order:
                        raw=int(pf["registers"][a])
                        try:
                            meta=SETTINGS.get(a)
                            if not meta or not meta.get("safe"): raise PermissionError("Register locked")
                            old=BUS.read(a)[0]
                            if old==raw:
                                results.append({"register":a,"old_raw":old,"new_raw":old,"ok":True,"skipped":True})
                                continue
                            validate(a,raw)
                            new=BUS.write_fc16(a,raw)
                            audit(a,old,new)
                            results.append({"register":a,"old_raw":old,"new_raw":new,"ok":True,"skipped":False})
                            time.sleep(.08)
                        except Exception as e:
                            results.append({"register":a,"ok":False,"error":str(e)})
                            break
                try: poll_once()
                except: pass
                ok=all(x.get("ok") for x in results) and len(results)==len(target_order)
                code=200 if ok else 409
                first_error=next((x for x in results if not x.get("ok")),None)
                err=(f"Profile stopped at register {first_error['register']}: {first_error.get('error','unknown error')}" if first_error else "")
                return self.J({"ok":ok,"profile":pid,"label":pf["label"],"results":results,"error":err},code)
            except PermissionError as e:return self.J({"error":str(e)},403)
            except Exception as e:return self.J({"error":str(e)},400)
        if path=="/api/time/set":
            try:
                if not ALLOW_WRITE: raise PermissionError("Global writes disabled")
                n=int(self.headers.get("Content-Length","0")); b=json.loads(self.rfile.read(n) or b"{}")
                vals=[int(b[k]) for k in ("year","month","day","hour","minute","second")]
                y,mo,d,h,mi,s=vals
                if not (2000<=y<=2099 and 1<=mo<=12 and 1<=d<=31 and 0<=h<=23 and 0<=mi<=59 and 0<=s<=59):
                    raise ValueError("RTC value outside allowed range")
                datetime(y,mo,d,h,mi,s)  # validates calendar date
                with BUS.lock:
                    q=BUS.write_block_fc16(434,vals)
                rtc="%04d-%02d-%02d %02d:%02d:%02d"%tuple(q)
                try: poll_once()
                except: pass
                return self.J({"ok":True,"rtc":rtc,"registers":"434..439","raw":q})
            except PermissionError as e:return self.J({"error":str(e)},403)
            except Exception as e:return self.J({"error":str(e)},400)
        if path!="/api/write":return self.J({"error":"Not found"},404)
        try:
            n=int(self.headers.get("Content-Length","0"));b=json.loads(self.rfile.read(n) or b"{}");a=int(b["register"]);u=float(b["value"]);meta=SETTINGS.get(a)
            if not meta:raise PermissionError("Unknown register")
            if not meta.get("safe"):raise PermissionError("Register locked")
            if meta["kind"]=="select":
                raw=int(u)
                if raw not in meta["values"]:raise ValueError("Invalid value")
            else:
                if u<meta["min"] or u>meta["max"]:raise ValueError("Outside v2.3 safety range")
                raw=int(round(u*meta.get("scale",1)))
            if a==351 and raw!=0 and not (50<=raw<=500):
                raise ValueError("Maximum battery discharge current: use 0 (OFF) or 50..500 A")
            with BUS.lock:old=BUS.read(a)[0];validate(a,raw);new=BUS.write_fc16(a,raw);audit(a,old,new)
            try:poll_once()
            except:pass
            return self.J({"ok":True,"register":a,"old_raw":old,"new_raw":new})
        except PermissionError as e:return self.J({"error":str(e)},403)
        except Exception as e:return self.J({"error":str(e)},400)

if __name__=="__main__":
    BUS=Bus();threading.Thread(target=poller,daemon=True).start()
    if BMS_ENABLE:
        bms_trigger_async()  # one initial BMS refresh even in manual mode
        if BMS_POLL_SEC>0:
            # Start periodic mode only after the initial request has been launched.
            threading.Thread(target=bms_worker,daemon=True).start()
    print("ANENJI v2.11.26 RS485:",PORT,BAUD,"8N1 slave",SLAVE,"poll",POLL_SEC,"web",HTTP_PORT,"writes",ALLOW_WRITE,"BMS",("on" if BMS_ENABLE else "off"),("MACs="+",".join(BMS_MACS) if BMS_MACS else "MACs-not-configured"),"BMS-poll",("manual" if BMS_POLL_SEC<=0 else str(BMS_POLL_SEC)+"s"),"BMS-io-timeout",BMS_IO_TIMEOUT_MS,"BMS-parallel",BMS_PARALLEL,"BMS-retry",BMS_RETRY_FAILED,"BMS-auto-discharge",BMS_AUTO_ON_DISCHARGE,"thresholdW",BMS_DISCHARGE_THRESHOLD_W,"discharge-poll",BMS_DISCHARGE_POLL_SEC,"reserveSOC",BATTERY_RESERVE_SOC,"topology",BMS_TOPOLOGY)
    try:ThreadingHTTPServer(("0.0.0.0",HTTP_PORT),H).serve_forever()
    finally:BUS.close()
