# Piano di Rimozione Funzionalità BambuLab e AMS

## Introduzione
Questo documento analizza tutte le funzionalità correlate a BambuLab (BBL) e AMS (Automatic Material System) nel codebase di GingerSlicer, fornendo un piano dettagliato per la loro rimozione passo passo.

## Panoramica delle Funzionalità da Rimuovere

### 1. Funzionalità di Rete BambuLab
- **NetworkAgent**: Sistema di comunicazione con stampanti BambuLab via MQTT/HTTP
- **bambu_networking**: Libreria di networking per BambuLab
- **DeviceManager**: Gestione dispositivi BambuLab
- **Bind/Unbind**: Sistema di binding stampanti all'account cloud
- **UserManager**: Gestione utenti BambuLab cloud
- **HttpServer**: Server locale per OAuth BambuLab

### 2. Funzionalità AMS (Automatic Material System)
- **AmsWidgets**: Widget UI per AMS
- **AMSMaterialsSetting**: Impostazioni materiali AMS
- **AMSSetting**: Impostazioni AMS generali
- **AmsMappingPopup**: Popup per mapping AMS
- **AMSControl/AMSItem**: Componenti controllo AMS
- **Logica AMS in DeviceManager**: Gestione stati AMS

### 3. UI Specifiche BambuLab
- **Monitor**: Pannello di monitoraggio stampanti BBL
- **BBLTopbar**: Barra superiore BBL
- **BBLStatusBar**: Barra di stato BBL (Bind, Send)
- **SendToPrinter**: Invio G-code a stampanti BBL
- **SelectMachine**: Selezione stampanti BBL
- **BindDialog**: Dialog di binding stampanti
- **WebUserLoginDialog**: Login web BambuLab
- **CameraPopup**: Popup camera BBL
- **UpgradePanel**: Pannello upgrade firmware BBL
- **HMSPanel**: Pannello HMS (Hazard Message System)
- **MediaFilePanel**: Pannello file media BBL
- **StatusPanel**: Pannello stato stampante BBL

### 4. File System e Streaming
- **PrinterFileSystem**: File system stampanti BBL
- **BambuTunnel**: Tunnel per streaming BBL
- **gstbambusrc**: GStreamer source per BBL
- **BambuPlayer**: Player video BBL

### 5. Job e Task
- **BindJob**: Job di binding stampanti
- **SendJob**: Job di invio G-code
- **PrintJob**: Job di stampa
- **TaskManager**: Gestione task BBL
- **MultiTaskManagerPage**: Gestione multi-task

### 6. Calibrazione Specifica BBL
- **CalibrationPanel**: Pannello calibrazione BBL
- **CalibrationWizard**: Wizard calibrazione BBL
- **ExtrusionCalibration**: Calibrazione estrusione BBL
- **CalibUtils**: Utility calibrazione BBL

### 7. Formato File
- **bbs_3mf**: Lettura/scrittura formato 3MF Bambu Studio

## Struttura del Piano di Rimozione

### FASE 1: Rimozione Dipendenze di Rete (Foundation)

#### 1.1 Rimuovere NetworkAgent e bambu_networking
**File da rimuovere:**
- `src/slic3r/Utils/bambu_networking.hpp`
- `src/slic3r/Utils/NetworkAgent.hpp`
- `src/slic3r/Utils/NetworkAgent.cpp`

**Impatto:**
- Rimozione completa del sistema di comunicazione BBL
- Necessario rimuovere tutti i riferimenti in GUI_App, DeviceManager, ecc.

#### 1.2 Rimuovere UserManager
**File da rimuovere:**
- `src/slic3r/GUI/UserManager.hpp`
- `src/slic3r/GUI/UserManager.cpp`

**Impatto:**
- Rimozione gestione utenti cloud BBL
- Rimuovere riferimenti in GUI_App

#### 1.3 Rimuovere HttpServer
**File da rimuovere:**
- `src/slic3r/GUI/HttpServer.hpp`
- `src/slic3r/GUI/HttpServer.cpp`

**Impatto:**
- Rimozione server locale OAuth
- Rimuovere riferimenti in GUI_App, OAuthJob

### FASE 2: Rimozione DeviceManager e Monitor

#### 2.1 Rimuovere DeviceManager
**File da rimuovere:**
- `src/slic3r/GUI/DeviceManager.hpp`
- `src/slic3r/GUI/DeviceManager.cpp`
- `src/slic3r/GUI/DeviceTab/` (intera directory)

**Impatto:**
- Rimozione gestione dispositivi BBL
- Rimuovere tutti i riferimenti in GUI_App, Monitor, Plater

#### 2.2 Rimuovere Monitor e pannelli correlati
**File da rimuovere:**
- `src/slic3r/GUI/Monitor.hpp`
- `src/slic3r/GUI/Monitor.cpp`
- `src/slic3r/GUI/MonitorBasePanel.h`
- `src/slic3r/GUI/MonitorBasePanel.cpp`
- `src/slic3r/GUI/MonitorPage.hpp`
- `src/slic3r/GUI/MonitorPage.cpp`
- `src/slic3r/GUI/StatusPanel.hpp`
- `src/slic3r/GUI/StatusPanel.cpp`
- `src/slic3r/GUI/UpgradePanel.hpp`
- `src/slic3r/GUI/UpgradePanel.cpp`
- `src/slic3r/GUI/HMSPanel.hpp`
- `src/slic3r/GUI/HMSPanel.cpp`
- `src/slic3r/GUI/MediaFilePanel.hpp`
- `src/slic3r/GUI/MediaFilePanel.cpp`
- `src/slic3r/GUI/CameraPopup.hpp`
- `src/slic3r/GUI/CameraPopup.cpp`

**Impatto:**
- Rimozione completa interfaccia monitoraggio BBL
- Rimuovere tab Device da MainFrame

### FASE 3: Rimozione UI BBL

#### 3.1 Rimuovere componenti BBL
**File da rimuovere:**
- `src/slic3r/GUI/BBLTopbar.hpp`
- `src/slic3r/GUI/BBLTopbar.cpp`
- `src/slic3r/GUI/BBLStatusBar.hpp`
- `src/slic3r/GUI/BBLStatusBar.cpp`
- `src/slic3r/GUI/BBLStatusBarBind.hpp`
- `src/slic3r/GUI/BBLStatusBarBind.cpp`
- `src/slic3r/GUI/BBLStatusBarSend.hpp`
- `src/slic3r/GUI/BBLStatusBarSend.cpp`

**Impatto:**
- Rimozione barre UI specifiche BBL
- Pulire MainFrame e GUI_App

#### 3.2 Rimuovere dialog e popup
**File da rimuovere:**
- `src/slic3r/GUI/BindDialog.hpp`
- `src/slic3r/GUI/BindDialog.cpp`
- `src/slic3r/GUI/SelectMachine.hpp`
- `src/slic3r/GUI/SelectMachine.cpp`
- `src/slic3r/GUI/SelectMachinePop.hpp`
- `src/slic3r/GUI/SelectMachinePop.cpp`
- `src/slic3r/GUI/WebUserLoginDialog.hpp`
- `src/slic3r/GUI/WebUserLoginDialog.cpp`
- `src/slic3r/GUI/SendToPrinter.hpp`
- `src/slic3r/GUI/SendToPrinter.cpp`
- `src/slic3r/GUI/SendMultiMachinePage.hpp`
- `src/slic3r/GUI/SendMultiMachinePage.cpp`
- `src/slic3r/GUI/MultiMachineManagerPage.hpp`
- `src/slic3r/GUI/MultiMachineManagerPage.cpp`
- `src/slic3r/GUI/MultiMachine.hpp`
- `src/slic3r/GUI/MultiMachine.cpp`

**Impatto:**
- Rimozione dialog interazione stampanti BBL
- Rimuovere menu e azioni correlate

### FASE 4: Rimozione AMS

#### 4.1 Rimuovere componenti AMS UI
**File da rimuovere:**
- `src/slic3r/GUI/AmsWidgets.hpp`
- `src/slic3r/GUI/AmsWidgets.cpp`
- `src/slic3r/GUI/AmsMappingPopup.hpp`
- `src/slic3r/GUI/AmsMappingPopup.cpp`
- `src/slic3r/GUI/AMSMaterialsSetting.hpp`
- `src/slic3r/GUI/AMSMaterialsSetting.cpp`
- `src/slic3r/GUI/AMSSetting.hpp`
- `src/slic3r/GUI/AMSSetting.cpp`
- `src/slic3r/GUI/Widgets/AMSControl.hpp`
- `src/slic3r/GUI/Widgets/AMSControl.cpp`
- `src/slic3r/GUI/Widgets/AMSItem.hpp`
- `src/slic3r/GUI/Widgets/AMSItem.cpp`
- `src/slic3r/GUI/DeviceTab/uiAmsHumidityPopup.h`
- `src/slic3r/GUI/DeviceTab/uiAmsHumidityPopup.cpp`

**Impatto:**
- Rimozione completa UI AMS
- Rimuovere riferimenti in Plater, GUI_ObjectList

#### 4.2 Rimuovere logica AMS da libslic3r
**File da modificare:**
- `src/libslic3r/PrintConfig.hpp` - rimuovere opzioni AMS
- `src/libslic3r/PrintConfig.cpp` - rimuovere opzioni AMS
- `src/libslic3r/Model.hpp` - rimuovere strutture AMS
- `src/libslic3r/Model.cpp` - rimuovere logica AMS
- `src/libslic3r/Print.hpp` - rimuovere logica AMS
- `src/libslic3r/Print.cpp` - rimuovere logica AMS

### FASE 5: Rimozione Job e Task

#### 5.1 Rimuovere Job
**File da rimuovere:**
- `src/slic3r/GUI/Jobs/BindJob.hpp`
- `src/slic3r/GUI/Jobs/BindJob.cpp`
- `src/slic3r/GUI/Jobs/SendJob.hpp`
- `src/slic3r/GUI/Jobs/SendJob.cpp`
- `src/slic3r/GUI/Jobs/PrintJob.hpp`
- `src/slic3r/GUI/Jobs/PrintJob.cpp`

**Impatto:**
- Rimozione job asincroni BBL
- Rimuovere riferimenti in GUI_App, Plater

#### 5.2 Rimuovere TaskManager
**File da rimuovere:**
- `src/slic3r/GUI/TaskManager.hpp`
- `src/slic3r/GUI/TaskManager.cpp`
- `src/slic3r/GUI/MultiTaskManagerPage.hpp`
- `src/slic3r/GUI/MultiTaskManagerPage.cpp`
- `src/slic3r/GUI/MultiTaskModel.hpp`
- `src/slic3r/GUI/MultiTaskModel.cpp`

**Impatto:**
- Rimozione gestione task cloud BBL

### FASE 6: Rimozione Calibrazione BBL

#### 6.1 Rimuovere pannelli calibrazione
**File da rimuovere:**
- `src/slic3r/GUI/CalibrationPanel.hpp`
- `src/slic3r/GUI/CalibrationPanel.cpp`
- `src/slic3r/GUI/Calibration.hpp`
- `src/slic3r/GUI/Calibration.cpp`
- `src/slic3r/GUI/CalibrationWizard.hpp`
- `src/slic3r/GUI/CalibrationWizard.cpp`
- `src/slic3r/GUI/CalibrationWizardStartPage.hpp`
- `src/slic3r/GUI/CalibrationWizardStartPage.cpp`
- `src/slic3r/GUI/CalibrationWizardPage.hpp`
- `src/slic3r/GUI/CalibrationWizardPage.cpp`
- `src/slic3r/GUI/CalibrationWizardCaliPage.hpp`
- `src/slic3r/GUI/CalibrationWizardCaliPage.cpp`
- `src/slic3r/GUI/CalibrationWizardPresetPage.hpp`
- `src/slic3r/GUI/CalibrationWizardPresetPage.cpp`
- `src/slic3r/GUI/CalibrationWizardSavePage.hpp`
- `src/slic3r/GUI/CalibrationWizardSavePage.cpp`
- `src/slic3r/GUI/ExtrusionCalibration.hpp`
- `src/slic3r/GUI/ExtrusionCalibration.cpp`
- `src/slic3r/GUI/CaliHistoryDialog.hpp`
- `src/slic3r/GUI/CaliHistoryDialog.cpp`

**Impatto:**
- Rimozione calibrazione specifica BBL
- Rimuovere riferimenti in MainFrame, Tab

#### 6.2 Rimuovere utility calibrazione
**File da rimuovere:**
- `src/slic3r/Utils/CalibUtils.hpp`
- `src/slic3r/Utils/CalibUtils.cpp`
- `src/libslic3r/calib.hpp`
- `src/libslic3r/calib.cpp`

**Impatto:**
- Rimozione logica calibrazione BBL
- Rimuovere riferimenti in PrintConfig

### FASE 7: Rimozione File System e Streaming

#### 7.1 Rimuovere PrinterFileSystem
**File da rimuovere:**
- `src/slic3r/GUI/Printer/PrinterFileSystem.hpp`
- `src/slic3r/GUI/Printer/PrinterFileSystem.cpp`

**Impatto:**
- Rimozione file system BBL
- Rimuovere riferimenti in GUI_App

#### 7.2 Rimuovere BambuTunnel e GStreamer
**File da rimuovere:**
- `src/slic3r/GUI/Printer/BambuTunnel.h`
- `src/slic3r/GUI/Printer/gstbambusrc.h`
- `src/slic3r/GUI/Printer/gstbambusrc.c`
- `src/slic3r/GUI/BambuPlayer/` (intera directory)

**Impatto:**
- Rimozione streaming video BBL
- Rimuovere dipendenze GStreamer da CMakeLists.txt

#### 7.3 Rimuovere WebView BBL
**File da rimuovere:**
- `src/slic3r/GUI/PrinterWebView.hpp`
- `src/slic3r/GUI/PrinterWebView.cpp`
- `src/slic3r/GUI/WebViewDialog.hpp`
- `src/slic3r/GUI/WebViewDialog.cpp`
- `src/slic3r/GUI/Widgets/WebView.hpp`
- `src/slic3r/GUI/Widgets/WebView.cpp`

**Impatto:**
- Rimozione WebView per contenuti BBL
- Rimuovere riferimenti in GUI_App

### FASE 8: Rimozione Formato bbs_3mf

#### 8.1 Rimuovere formato BBS 3MF
**File da rimuovere:**
- `src/libslic3r/Format/bbs_3mf.hpp`
- `src/libslic3r/Format/bbs_3mf.cpp`

**Impatto:**
- Rimozione lettura/scrittura 3MF Bambu Studio
- Rimuovere riferimenti in GUI_App, Model

### FASE 9: Pulizia CMakeLists.txt

#### 9.1 Rimuovere file BBL da CMakeLists.txt
**File da modificare:**
- `src/slic3r/CMakeLists.txt`

**Rimozioni:**
```cmake
# Rimuovere queste linee:
GUI/BBLStatusBarBind.cpp
GUI/BBLStatusBarBind.hpp
GUI/BBLStatusBar.cpp
GUI/BBLStatusBar.hpp
GUI/BBLStatusBarSend.cpp
GUI/BBLStatusBarSend.hpp
GUI/BBLTopbar.cpp
GUI/BBLTopbar.hpp
GUI/AmsMappingPopup.cpp
GUI/AmsMappingPopup.hpp
GUI/AMSMaterialsSetting.cpp
GUI/AMSMaterialsSetting.hpp
GUI/AMSSetting.cpp
GUI/AMSSetting.hpp
GUI/AmsWidgets.cpp
GUI/AmsWidgets.hpp
Utils/bambu_networking.hpp
GUI/Printer/gstbambusrc.c
```

**Rimozione GStreamer:**
```cmake
# Rimuovere queste sezioni:
# We add GStreamer for bambu:/// support.
find_package(GStreamer 1.0 REQUIRED COMPONENTS app base)
# ... tutte le dipendenze GStreamer
```

**Rimozione DeviceTab:**
```cmake
# Rimuovere:
add_subdirectory(DeviceTab)
```

### FASE 10: Pulizia GUI_App

#### 10.1 Rimuovere membri BBL da GUI_App
**File da modificare:**
- `src/slic3r/GUI/GUI_App.hpp`
- `src/slic3r/GUI/GUI_App.cpp`

**Rimozioni:**
```cpp
// Rimuovere questi membri:
NetworkAgent* m_agent { nullptr };
Slic3r::DeviceManager* m_device_manager { nullptr };
Slic3r::UserManager* m_user_manager { nullptr };
HttpServer m_http_server;
BBLTopbar* m_topbar { nullptr };

// Rimuovere questi metodi:
NetworkAgent* getAgent() { return m_agent; }
Slic3r::DeviceManager* getDeviceManager() { return m_device_manager; }
```

**Rimozioni includes:**
```cpp
// Rimuovere:
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/UserManager.hpp"
#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/GUI/BBLTopbar.hpp"
```

### FASE 11: Pulizia MainFrame

#### 11.1 Rimuovere riferimenti BBL da MainFrame
**File da modificare:**
- `src/slic3r/GUI/MainFrame.hpp`
- `src/slic3r/GUI/MainFrame.cpp`

**Rimozioni:**
```cpp
// Rimuovere:
void show_device(bool bBBLPrinter);
enum { eSendToPrinter = 5, eSendToPrinterAll = 6 };
```

**Rimozioni includes:**
```cpp
// Rimuovere:
#include "BBLTopbar.hpp"
#include "DeviceManager.hpp"
#include "Monitor.hpp"
#include "SendToPrinter.hpp"
#include "SelectMachine.hpp"
#include "BindDialog.hpp"
#include "CalibrationWizard.hpp"
#include "CalibrationPanel.hpp"
```

### FASE 12: Pulizia Plater

#### 12.1 Rimuovere riferimenti BBL da Plater
**File da modificare:**
- `src/slic3r/GUI/Plater.hpp`
- `src/slic3r/GUI/Plater.cpp`

**Rimozioni:**
```cpp
// Rimuovere:
SendToPrinterDialog* m_send_to_sdcard_dlg;
```

**Rimozioni includes:**
```cpp
// Rimuovere:
#include "SendToPrinter.hpp"
#include "AmsWidgets.hpp"
```

### FASE 13: Pulizia Tab

#### 13.1 Rimuovere riferimenti AMS/BBL da Tab
**File da modificare:**
- `src/slic3r/GUI/Tab.hpp`
- `src/slic3r/GUI/Tab.cpp`

**Rimozioni:**
- Rimuovere pagine AMS
- Rimuovere opzioni di configurazione AMS/BBL
- Rimuovere riferimenti a calibrazione BBL

### FASE 14: Pulizia Configurazione

#### 14.1 Rimuovere opzioni BBL da PrintConfig
**File da modificare:**
- `src/libslic3r/PrintConfig.hpp`
- `src/libslic3r/PrintConfig.cpp`

**Opzioni da rimuovere (esempio):**
- `ams_mapping`
- `ams_mapping_info`
- `ams_mapping2`
- `task_use_ams`
- Tutte le opzioni specifiche BBL/AMS

#### 14.2 Rimuovere opzioni BBL da AppConfig
**File da modificare:**
- `src/libslic3r/AppConfig.hpp`
- `src/libslic3r/AppConfig.cpp`

**Rimozioni:**
- Rimuovere configurazioni cloud BBL
- Rimuovere configurazioni device BBL
- Rimuovere configurazioni AMS

### FASE 15: Pulizia Risorse

#### 15.1 Rimuovere risorse web BBL
**Directory da rimuovere:**
- `resources/web/` (se contiene solo contenuti BBL)

**File da rimuovere:**
- `resources/web/login/` (login BambuLab)
- `resources/web/homepage/` (homepage BambuLab)

### FASE 16: Pulizia Dipendenze Esterne

#### 16.1 Rimuovere dipendenze da deps/
**Controllare e rimuovere:**
- Dipendenze GStreamer (se usate solo per BBL)
- Altre librerie specifiche BBL

### FASE 17: Pulizia Eventi e Callback

#### 17.1 Rimuovere eventi BBL
**File da modificare:**
- `src/slic3r/GUI/Event.hpp`

**Rimozioni:**
```cpp
// Rimuovere eventi come:
EVT_UPDATE_USER_MACHINE_LIST
EVT_PRINT_JOB_CANCEL
EVT_BIND_SUCCESS
EVT_BIND_FAIL
// ... tutti gli eventi BBL
```

### FASE 18: Pulizia Preset

#### 18.1 Rimuovere preset BBL da resources/
**Directory da controllare:**
- `resources/profiles/`

**Azione:**
- Rimuovere eventuali preset specifici BBL (se presenti)
- Mantenere solo preset Ginger Additive

### FASE 19: Verifica e Test

#### 19.1 Compilazione
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target GingerSlicer --config Release
```

#### 19.2 Test funzionalità
- Verificare che l'applicazione si avvii senza crash
- Verificare che le funzionalità di slicing funzionino
- Verificare che l'export G-code funzioni
- Verificare che l'import 3MF standard funzionino

### FASE 20: Documentazione

#### 20.1 Aggiornare documentazione
- Aggiornare README.md
- Rimuovere riferimenti a BambuLab dalla documentazione
- Aggiornare CHANGELOG con note di rimozione

## Ordine di Esecuzione Raccomandato

1. **FASE 1**: Rimozione dipendenze di rete (Foundation)
2. **FASE 2**: Rimozione DeviceManager e Monitor
3. **FASE 3**: Rimozione UI BBL
4. **FASE 4**: Rimozione AMS
5. **FASE 5**: Rimozione Job e Task
6. **FASE 6**: Rimozione Calibrazione BBL
7. **FASE 7**: Rimozione File System e Streaming
8. **FASE 8**: Rimozione Formato bbs_3mf
9. **FASE 9**: Pulizia CMakeLists.txt
10. **FASE 10**: Pulizia GUI_App
11. **FASE 11**: Pulizia MainFrame
12. **FASE 12**: Pulizia Plater
13. **FASE 13**: Pulizia Tab
14. **FASE 14**: Pulizia Configurazione
15. **FASE 15**: Pulizia Risorse
16. **FASE 16**: Pulizia Dipendenze Esterne
17. **FASE 17**: Pulizia Eventi e Callback
18. **FASE 18**: Pulizia Preset
19. **FASE 19**: Verifica e Test
20. **FASE 20**: Documentazione

## Note Importanti

1. **Backup**: Fare un backup completo prima di iniziare
2. **Commit incrementali**: Commit dopo ogni fase completata
3. **Test intermedi**: Testare la compilazione dopo ogni fase
4. **Dipendenze incrociate**: Alcuni file potrebbero avere dipendenze non evidenti
5. **Riferimenti rimanenti**: Potrebbero esserci riferimenti in file non ancora identificati

## Riepilogo File da Rimuovere

### Totale file stimati: ~80-100 file

### Categorie:
- Utils: 3 file
- GUI/DeviceManager: 2 file + 1 directory
- GUI/Monitor: 12 file
- GUI/BBL*: 8 file
- GUI/AMS*: 14 file
- GUI/Dialog: 15 file
- GUI/Jobs: 6 file
- GUI/Task: 6 file
- GUI/Calibration: 18 file
- GUI/Printer: 5 file + 1 directory
- GUI/WebView: 6 file
- GUI/Widgets: 4 file
- libslic3r/Format: 2 file
- libslic3r/Config: 4 file
- libslic3r/Calib: 2 file
- resources/web: directory

## Prossimi Passi

1. Iniziare con FASE 1 (rimozione NetworkAgent)
2. Procedere fase per fase
3. Testare dopo ogni fase
4. Documentare eventuali problemi
5. Adattare il piano se necessario
