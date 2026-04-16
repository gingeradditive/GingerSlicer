# Fase 5: Rimozione Backend BambuLab - Stato del Lavoro

**Data:** 16 Aprile 2026  
**Obiettivo:** Rimuovere UserManager, HttpServer, DeviceManager, NetworkAgent e tutti i riferimenti BambuLab specifici

## Riepilogo delle Attività Completate

### 1. File Eliminati (55 file totali)

**Target Fase 5 (6 file):**
- `slic3r/GUI/UserManager.hpp/cpp`
- `slic3r/GUI/HttpServer.hpp/cpp`
- `slic3r/GUI/DeviceManager.hpp/cpp`
- `slic3r/Utils/NetworkAgent.hpp/cpp`
- `slic3r/Utils/bambu_networking.hpp`
- `slic3r/GUI/TaskManager.hpp/cpp`

**File orfani Bambu-only (49 file):**

*Calibration Wizard (14 file):*
- `slic3r/GUI/Calibration.hpp/cpp`
- `slic3r/GUI/CalibrationWizard.hpp/cpp`
- `slic3r/GUI/CalibrationWizardPage.hpp/cpp`
- `slic3r/GUI/CalibrationWizardPresetPage.hpp/cpp`
- `slic3r/GUI/CalibrationWizardCaliPage.hpp/cpp`
- `slic3r/GUI/CalibrationWizardSavePage.hpp/cpp`
- `slic3r/GUI/CalibrationWizardStartPage.hpp/cpp`
- `slic3r/GUI/CaliHistoryDialog.hpp/cpp`

*Altri Bambu-specific (13 file):*
- `slic3r/GUI/ExtrusionCalibration.hpp/cpp`
- `slic3r/GUI/ConnectPrinter.hpp/cpp`
- `slic3r/GUI/PrintOptionsDialog.hpp/cpp`
- `slic3r/Utils/CalibUtils.hpp/cpp`
- `slic3r/GUI/Widgets/FanControl.hpp/cpp`
- `slic3r/GUI/Widgets/SideTools.hpp/cpp`
- `slic3r/GUI/MediaPlayCtrl.h/cpp`
- `slic3r/GUI/HMS.hpp/cpp`
- `slic3r/GUI/ImageGrid.h/cpp`
- `slic3r/GUI/SliceInfoPanel.hpp/cpp`
- `slic3r/GUI/wxMediaCtrl2.h/cpp`

*DeviceTab (3 file):*
- `slic3r/GUI/DeviceTab/uiDeviceUpdateVersion.h/cpp`
- `slic3r/GUI/DeviceTab/CMakeLists.txt` (directory intera da verificare)

*Printer (4 file):*
- `slic3r/GUI/Printer/PrinterFileSystem.h/cpp`
- `slic3r/GUI/Printer/BambuTunnel.h`
- `slic3r/GUI/Printer/gstbambusrc.h/c`

### 2. Pulizia GUI_App.hpp (IN CORSO - 80% completato)

**Completato:**
- ✅ Rimossi includes: `DeviceManager.hpp`, `NetworkAgent.hpp`, `HMS.hpp`, `HttpServer.hpp`, `UpgradeNetworkJob.hpp`
- ✅ Rimossi forward declarations: `UserManager`, `DeviceManager`, `NetworkAgent`, `HMSQuery`, `NetworkErrorDialog`
- ✅ Rimossi member variables:
  - `m_device_manager`
  - `m_user_manager`
  - `m_agent`
  - `hms_query`
  - `m_http_server`
  - `m_upgrade_network_job`
  - `m_networking_compatible`, `m_networking_need_update`, `m_networking_cancel_update`
  - `m_check_network_thread`
- ✅ Rimossi metodi:
  - `getDeviceManager()`
  - `get_hms_query()`
  - `getAgent()`
  - `request_user_login()`, `request_user_handle()`, `request_user_logout()`, `request_user_unbind()`
  - `on_set_selected_machine()`, `on_update_machine_list()`
  - `on_user_login()`, `on_user_login_handle()`
  - `enable_user_preset_folder()`
  - `process_network_msg()`
  - `start_http_server()`, `stop_http_server()`
  - `switch_staff_pick()`
  - `on_show_check_privacy_dlg()`, `show_check_privacy_dlg()`, `on_check_privacy_update()`, `check_privacy_update()`, `check_privacy_version()`, `check_track_enable()`
  - `updating_bambu_networking()`, `check_networking_version()`, `is_compatibility_version()`, `cancel_networking_install()`, `restart_networking()`
  - `copy_network_if_available()`, `init_networking_callbacks()`
  - `init_http_extra_header()`, `update_http_extra_header()`, `get_extra_header()`
  - `remove_old_networking_plugins()`
  - `get_plugin_url()`, `download_plugin()`, `install_plugin()`, `is_compatibility_version()`, `cancel_networking_install()`, `restart_networking()`
  - `m_server_error_dialog` member

**Da completare in GUI_App.hpp:**
- ⏳ Verificare se ci sono altri metodi rimasti che dipendono dalle classi eliminate

### 3. Pulizia GUI_App.cpp (IN CORSO - 20% completato)

**Completato:**
- ✅ Rimosso include: `UserManager.hpp`
- ✅ Pulito `OnExit()`: rimossi delete di `m_device_manager`, `m_user_manager`, `m_agent`
- ✅ Stub `on_init_network()`: rimossa intera funzione (tutta la logica di inizializzazione networking Bambu)

**Da completare in GUI_App.cpp:**
- ⏳ Pulire `post_init()` - contiene riferimenti a `hms_query`, `m_agent`, `m_device_manager`, `m_networking_need_update`, `NetworkAgent::get_version()`, `DeviceManager::load_filaments_blacklist_config()`
- ⏳ Rimuovere/stubare funzioni Bambu-specifiche:
  - `init_networking_callbacks()`
  - `restart_networking()`
  - `updating_bambu_networking()`
  - `check_networking_version()`
  - `is_compatibility_version()`
  - `cancel_networking_install()`
  - `copy_network_if_available()`
  - `init_http_extra_header()`, `update_http_extra_header()`, `get_extra_header()`
  - `remove_old_networking_plugins()`
  - `on_start_subscribe_again()`
  - `process_network_msg()`
  - `request_user_login()`, `request_user_handle()`, `request_user_logout()`, `request_user_unbind()`
  - `on_user_login()`, `on_user_login_handle()`
  - `on_set_selected_machine()`, `on_update_machine_list()`
  - `enable_user_preset_folder()`
  - `check_track_enable()`
  - `on_show_check_privacy_dlg()`, `show_check_privacy_dlg()`, `on_check_privacy_update()`, `check_privacy_update()`, `check_privacy_version()`
  - `start_http_server()`, `stop_http_server()`
  - `switch_staff_pick()`
  - `get_plugin_url()`, `download_plugin()`, `install_plugin()`
  - `show_ip_address_enter_dialog()` (dipende da MachineObject)
  - `on_show_check_privacy_dlg_handler()`
- ⏳ Pulire riferimenti sparsi in altre funzioni:
  - `request_login()`, `is_user_login()`, `check_login()`
  - `get_login_info()`
  - `delete_preset_from_cloud()`, `preset_deleted_from_cloud()`
  - `remove_user_presets()`
  - `sync_preset()`, `start_sync_user_preset()`, `stop_sync_user_preset()`
  - Altri riferimenti a `m_agent`, `m_device_manager`, `hms_query`, `NetworkAgent`, `DeviceManager`

## Attività Pendenti (in ordine di priorità)

### 1. Completare pulizia GUI_App.cpp (CRITICO)
- Pulire `post_init()` (righe ~799-1030)
- Rimuovere/stubare tutte le funzioni Bambu-specifiche elencate sopra
- Pulire riferimenti sparsi nelle altre funzioni

### 2. Pulire altri file che includono le classi eliminate (CRITICO)

File da pulire:
- `slic3r/GUI/Plater.hpp/cpp` - rimuovere include DeviceManager, riferimenti a MachineObject
- `slic3r/GUI/MainFrame.cpp` - rimuovere riferimenti a NetworkAgent (getAgent(), track_enable)
- `slic3r/GUI/BBLTopbar.hpp/cpp` - rimuovere include DeviceManager
- `slic3r/GUI/PresetComboBoxes.cpp` - rimuovere include CalibrationWizardPage, CalibrationWizardPresetPage
- `slic3r/GUI/Auxiliary.hpp/cpp` - rimuovere include DeviceManager, SideTools
- `slic3r/GUI/ReleaseNote.hpp/cpp` - rimuovere include DeviceManager, stub InputIpAddressDialog
- `slic3r/GUI/WebViewDialog.hpp/cpp` - rimuovere forward declaration NetworkAgent, stub OpenModelDetail, SendLoginInfo
- `slic3r/GUI/GLCanvas3D.cpp` - rimuovere riferimenti a NetworkAgent
- `slic3r/GUI/Preferences.cpp` - rimuovere riferimenti a NetworkAgent
- `slic3r/Utils/PresetUpdater.cpp` - rimuovere riferimenti a bambu_networking
- `slic3r/GUI/CreatePresetsDialog.cpp` - rimuovere riferimenti a NetworkAgent
- `slic3r/GUI/DownloadProgressDialog.hpp/cpp` - rimuovere riferimenti a UpgradeNetworkJob
- `slic3r/GUI/Jobs/OAuthJob.hpp/cpp` - rimuovere include HttpServer
- `slic3r/GUI/BBLStatusBarSend.hpp/cpp` - rimuovere riferimenti a UpgradeNetworkJob

### 3. Aggiornare CMakeLists.txt
- Rimuovere tutte le voci dei file eliminati
- Verificare che non ci siano dipendenze orfane

### 4. Verifica finale
- Ricerca globale per simboli orfani (DeviceManager, NetworkAgent, UserManager, HMS, HttpServer, TaskManager)
- Build di test
- Notificare l'utente quando buildabile

## Dove mi sono interrotto

Stavo pulendo `GUI_App.cpp`. Ho completato:
- Rimozione include UserManager.hpp
- Pulizia OnExit() 
- Stub di on_init_network()

Il prossimo passo immediato è continuare a pulire GUI_App.cpp:
1. Leggere e pulire `post_init()` (righe 799-1030)
2. Rimuovere/stubare le funzioni Bambu-specifiche (init_networking_callbacks, restart_networking, etc.)
3. Pulire riferimenti sparsi nelle altre funzioni

## Note Importanti

- **BBLStatusBarSend** e **BBLStatusBar** devono essere mantenuti (usati da DownloadProgressDialog, ReleaseNote, MsgDialog)
- **WebViewDialog** deve essere mantenuto ma stubare i metodi che usano NetworkAgent
- **ReleaseNote** ha InputIpAddressDialog che dipende da DeviceManager - da stubare o rimuovere
- **calib_dlg.hpp** (SoftFever calibration dialogs) è indipendente da DeviceManager e deve essere mantenuto
- Il file GUI_App.cpp è enorme (~6700 righe) con molti riferimenti sparsi alle classi eliminate

## Comando per riprendere

Per continuare il lavoro, il prossimo comando da eseguire è:
```bash
# Leggere post_init() in GUI_App.cpp per vedere i riferimenti da pulire
sed -n '799,1030p' /home/jack--/Sources/GingerRepos/GingerSlicer/src/slic3r/GUI/GUI_App.cpp
```

Poi procedere con la pulizia sistematica delle funzioni elencate sopra.
