# Piano di Rimozione Funzionalità BambuLab e AMS

## Scopo del documento

Questo file deve guidare una IA nella rimozione progressiva delle funzionalità usate solo da stampanti BambuLab e delle funzionalità AMS.

L'obiettivo non è cancellare tutto ciò che contiene i prefissi `BBL`, `Bambu`, `AMS` o `Ams`, ma rimuovere solo ciò che è davvero specifico a:

- cloud BambuLab
- binding/login/account BambuLab
- gestione device BambuLab
- invio stampa verso device BambuLab
- monitoraggio remoto BambuLab
- streaming/camera/file browser BambuLab
- AMS e relative UI/configurazioni

## Vincoli obbligatori

- **Non toccare l'About.**
- **Non rimuovere `BBLTopbar`.** Anche se il nome contiene `BBL`, oggi è usata come top bar generale del `MainFrame`.
- **Non assumere che ogni `BBLStatusBar*` sia Bambu-only.** Alcune classi sono componenti UI riusate anche fuori dai flussi Bambu stretti.
- **Ogni step deve produrre una build compilabile o quasi compilabile con una pulizia locale molto limitata.**
- **Dopo ogni step eliminare include, forward declaration, enum, eventi e binding non più usati.**
- **Non partire dai file di supporto condivisi.** Prima rimuovere i consumatori specifici, poi le dipendenze residue.

## Elementi sicuramente fuori scope

- `src/slic3r/GUI/BBLTopbar.hpp`
- `src/slic3r/GUI/BBLTopbar.cpp`
- qualunque codice `AboutDialog`, `about()`, `create_about(...)`
- contenuti informativi o schermate About presenti nei wizard, anche se menzionano BambuLab

## Strategia generale

L'ordine corretto è:

1. rimuovere UI e workflow terminali Bambu/AMS
2. rimuovere integrazioni `MainFrame`, `Plater`, `Tab`, `GUI_App`
3. rimuovere servizi e manager non più referenziati
4. rimuovere backend networking e streaming Bambu
5. rimuovere configurazioni, eventi, CMake e risorse residue

Questo ordine evita di cancellare troppo presto componenti condivisi o classi ancora usate.

## Checklist di analisi prima di ogni rimozione

Per ogni funzione o file da rimuovere, la IA deve verificare sempre:

- se è usata solo da flow Bambu/AMS
- se compare in `MainFrame`, `Plater`, `GUI_App`, `Tab`, `Monitor`, `Calibration*`
- se la classe ha nome BBL ma comportamento generico
- se serve solo come widget di progresso o UI comune
- se ci sono stringhe o opzioni config corrispondenti in `PrintConfig`, `AppConfig`, preset o risorse

## Piano di rimozione passo-passo

## Fase 1 - Rimuovere i workflow utente esplicitamente Bambu

### Step 1 - Rimuovere il dialog di login web BambuLab

- **Target**: `src/slic3r/GUI/WebUserLoginDialog.hpp`, `src/slic3r/GUI/WebUserLoginDialog.cpp`
- **Motivo**: usa `NetworkAgent::get_bambulab_host()` e il login cloud BambuLab.
- **Da aggiornare**:
  - menu o azioni che aprono il login
  - chiamate in `GUI_App` o flussi di autenticazione
- **Non toccare**:
  - finestre web generiche non dedicate a login Bambu

### Step 2 - Rimuovere il workflow di bind stampante

- **Target**: `src/slic3r/GUI/BindDialog.hpp`, `src/slic3r/GUI/BindDialog.cpp`
- **Motivo**: dialog dedicato a bind/unbind verso ecosistema BambuLab.
- **Da aggiornare**:
  - azioni da menu e toolbar che aprono il bind
  - callback di successo/fallimento bind
  - job o worker collegati
- **Nota**: se usa `BBLStatusBarBind`, non rimuovere subito la status bar; rimuovere prima il dialog consumatore.

### Step 3 - Rimuovere la selezione macchina Bambu per invio stampa

- **Target**: `src/slic3r/GUI/SelectMachine.hpp`, `src/slic3r/GUI/SelectMachine.cpp`
- **Motivo**: è parte del flow di selezione device Bambu per invio/remoto.
- **Da aggiornare**:
  - entry point da `Plater`, `MainFrame`, menu contestuali
  - worker di send/print collegati
- **Attenzione**:
  - `BBLTopbar.hpp` include `SelectMachine.hpp`; dopo la rimozione va ripulita l'inclusione se non più necessaria, ma **la topbar resta**.

### Step 4 - Rimuovere popup e varianti del flow Select Machine

- **Target**: `src/slic3r/GUI/SelectMachinePop.hpp`, `src/slic3r/GUI/SelectMachinePop.cpp`
- **Motivo**: variante dello stesso workflow Bambu.
- **Da aggiornare**:
  - riferimenti da `SendToPrinter`, `SelectMachine`, `MainFrame`

### Step 5 - Rimuovere il dialog di invio stampa a device Bambu

- **Target**: `src/slic3r/GUI/SendToPrinter.hpp`, `src/slic3r/GUI/SendToPrinter.cpp`
- **Motivo**: invio G-code/stampa verso stampanti connessi via stack Bambu.
- **Da aggiornare**:
  - azioni `send to printer`
  - pulsanti e menu nel `MainFrame`
  - riferimenti in `Plater`
- **Nota**: se usa `BBLStatusBarSend`, non cancellarla ancora se ancora referenziata altrove.

### Step 6 - Rimuovere la pagina multi-macchina di invio

- **Target**: `src/slic3r/GUI/SendMultiMachinePage.hpp`, `src/slic3r/GUI/SendMultiMachinePage.cpp`
- **Motivo**: UI dedicata all'invio a più device con stato AMS.
- **Da aggiornare**:
  - dialog o container padre
  - sort, colonne AMS, refresh device

### Step 7 - Rimuovere i flussi multi-device Bambu residui

- **Target candidati**:
  - `src/slic3r/GUI/MultiMachine.hpp`
  - `src/slic3r/GUI/MultiMachine.cpp`
  - `src/slic3r/GUI/MultiMachineManagerPage.hpp`
  - `src/slic3r/GUI/MultiMachineManagerPage.cpp`
  - `src/slic3r/GUI/MultiTaskManagerPage.hpp`
  - `src/slic3r/GUI/MultiTaskManagerPage.cpp`
  - `src/slic3r/GUI/MultiTaskModel.hpp`
  - `src/slic3r/GUI/MultiTaskModel.cpp`
- **Motivo**: gestione cloud/device multipli fortemente legata a Bambu ecosystem.
- **Da fare prima**: confermare che non siano riusati da funzioni Ginger generiche.

## Fase 2 - Rimuovere tutta la UI AMS

### Step 8 - Rimuovere il popup di mapping AMS

- **Target**: `src/slic3r/GUI/AmsMappingPopup.hpp`, `src/slic3r/GUI/AmsMappingPopup.cpp`
- **Motivo**: funzione esclusivamente AMS.
- **Da aggiornare**:
  - chiamanti in `AMSControl`, `AMSItem`, `Plater`, sidebar device

### Step 9 - Rimuovere le impostazioni AMS principali

- **Target**: `src/slic3r/GUI/AMSSetting.hpp`, `src/slic3r/GUI/AMSSetting.cpp`
- **Motivo**: dialog dedicato a opzioni AMS come auto read, backup spool, remain capacity.
- **Da aggiornare**:
  - pulsanti impostazioni AMS
  - callback `EVT_AMS_SETTINGS`

### Step 10 - Rimuovere le impostazioni materiali AMS

- **Target**: `src/slic3r/GUI/AMSMaterialsSetting.hpp`, `src/slic3r/GUI/AMSMaterialsSetting.cpp`
- **Motivo**: gestione materiali e tray AMS.
- **Da aggiornare**:
  - aperture da popup o monitor
  - riferimenti in `StatusPanel` o componenti device

### Step 11 - Rimuovere il widget principale `AMSControl`

- **Target**: `src/slic3r/GUI/Widgets/AMSControl.hpp`, `src/slic3r/GUI/Widgets/AMSControl.cpp`
- **Motivo**: widget centrale di visualizzazione e comando AMS.
- **Da aggiornare**:
  - `StatusPanel`
  - `DeviceTab`
  - eventuali pannelli monitor che lo ospitano

### Step 12 - Rimuovere i widget `AMSItem` e sottocomponenti grafici AMS

- **Target**: `src/slic3r/GUI/Widgets/AMSItem.hpp`, `src/slic3r/GUI/Widgets/AMSItem.cpp`
- **Motivo**: interfaccia slot/tray/road/humidity/preview interamente AMS.
- **Da aggiornare**:
  - eventi `EVT_AMS_*`
  - include di `AMSItem.hpp`

### Step 13 - Rimuovere `AmsWidgets`

- **Target**: `src/slic3r/GUI/AmsWidgets.hpp`, `src/slic3r/GUI/AmsWidgets.cpp`
- **Motivo**: wrapper o componentistica di supporto AMS.
- **Da aggiornare**:
  - `Plater`
  - sidebar o pagine che caricano lista AMS

### Step 14 - Rimuovere popup umidità AMS dal device tab

- **Target**:
  - `src/slic3r/GUI/DeviceTab/uiAmsHumidityPopup.h`
  - `src/slic3r/GUI/DeviceTab/uiAmsHumidityPopup.cpp`
- **Motivo**: funzione esclusivamente AMS.

## Fase 3 - Rimuovere monitor e pannelli device Bambu

### Step 15 - Rimuovere `StatusPanel`

- **Target**: `src/slic3r/GUI/StatusPanel.hpp`, `src/slic3r/GUI/StatusPanel.cpp`
- **Motivo**: pannello di stato device con forte integrazione AMS e macchina remota.
- **Da aggiornare**:
  - `Monitor`
  - `DeviceTab`
  - eventuali tab notebook del monitor

### Step 16 - Rimuovere `UpgradePanel`

- **Target**: `src/slic3r/GUI/UpgradePanel.hpp`, `src/slic3r/GUI/UpgradePanel.cpp`
- **Motivo**: upgrade firmware device Bambu.

### Step 17 - Rimuovere `HMSPanel`

- **Target**: `src/slic3r/GUI/HMSPanel.hpp`, `src/slic3r/GUI/HMSPanel.cpp`
- **Motivo**: diagnostica/HMS device remoti Bambu.

### Step 18 - Rimuovere `MediaFilePanel`

- **Target**: `src/slic3r/GUI/MediaFilePanel.hpp`, `src/slic3r/GUI/MediaFilePanel.cpp`
- **Motivo**: browser media/file remoto del device.

### Step 19 - Rimuovere `CameraPopup`

- **Target**: `src/slic3r/GUI/CameraPopup.hpp`, `src/slic3r/GUI/CameraPopup.cpp`
- **Motivo**: preview camera/stream remota Bambu.

### Step 20 - Rimuovere `Monitor` e pannelli contenitore

- **Target candidati**:
  - `src/slic3r/GUI/Monitor.hpp`
  - `src/slic3r/GUI/Monitor.cpp`
  - `src/slic3r/GUI/MonitorBasePanel.h`
  - `src/slic3r/GUI/MonitorBasePanel.cpp`
  - `src/slic3r/GUI/MonitorPage.hpp`
  - `src/slic3r/GUI/MonitorPage.cpp`
- **Motivo**: contenitore principale del monitor device.
- **Da aggiornare**:
  - `MainFrame`
  - creazione tab/pagine device

## Fase 4 - Rimuovere i job e le progress bar rimaste solo se non più usate

### Step 21 - Rimuovere `BindJob`

- **Target**: `src/slic3r/GUI/Jobs/BindJob.hpp`, `src/slic3r/GUI/Jobs/BindJob.cpp`
- **Precondizione**: `BindDialog` già rimosso.

### Step 22 - Rimuovere `SendJob`

- **Target**: `src/slic3r/GUI/Jobs/SendJob.hpp`, `src/slic3r/GUI/Jobs/SendJob.cpp`
- **Precondizione**: `SendToPrinter` e multi-machine send già rimossi.

### Step 23 - Rimuovere `PrintJob` se usato solo dal flusso Bambu

- **Target**: `src/slic3r/GUI/Jobs/PrintJob.hpp`, `src/slic3r/GUI/Jobs/PrintJob.cpp`
- **Attenzione**: verificare che non sia usato da workflow locali non Bambu.

### Step 24 - Rimuovere `BBLStatusBarBind` solo dopo la scomparsa dei consumatori

- **Target**: `src/slic3r/GUI/BBLStatusBarBind.hpp`, `src/slic3r/GUI/BBLStatusBarBind.cpp`
- **Precondizione**: nessun riferimento da `BindDialog` o altri dialog.

### Step 25 - Rimuovere `BBLStatusBarSend` solo dopo la scomparsa dei consumatori

- **Target**: `src/slic3r/GUI/BBLStatusBarSend.hpp`, `src/slic3r/GUI/BBLStatusBarSend.cpp`
- **Attenzione**: attualmente compare anche in pagine di calibrazione; non rimuoverla finché non si decide cosa fare di quei flussi.

### Step 26 - Valutare `BBLStatusBar` base

- **Target**: `src/slic3r/GUI/BBLStatusBar.hpp`, `src/slic3r/GUI/BBLStatusBar.cpp`
- **Regola**: rimuoverla solo se dopo gli step precedenti non esistono più usi reali.
- **Motivo del rinvio**: il nome è fuorviante; la classe sembra una progress/status UI abbastanza generica.

## Fase 5 - Rimuovere il backend device/cloud Bambu

### Step 27 - Rimuovere `UserManager`

- **Target**: `src/slic3r/GUI/UserManager.hpp`, `src/slic3r/GUI/UserManager.cpp`
- **Precondizione**: login, bind, sincronizzazioni cloud e funzioni account già scollegate.

### Step 28 - Rimuovere `HttpServer`

- **Target**: `src/slic3r/GUI/HttpServer.hpp`, `src/slic3r/GUI/HttpServer.cpp`
- **Precondizione**: nessun flusso OAuth/login residuo.

### Step 29 - Rimuovere `DeviceManager`

- **Target**: `src/slic3r/GUI/DeviceManager.hpp`, `src/slic3r/GUI/DeviceManager.cpp`
- **Precondizione**:
  - monitor rimosso
  - send/bind/login rimossi
  - AMS UI rimossa
  - `GUI_App` non deve più chiamare `getDeviceManager()` nei flussi ordinari
- **Impatto**: è uno degli step più grandi, da fare tardi.

### Step 30 - Rimuovere `NetworkAgent` e l'header `bambu_networking.hpp`

- **Target**:
  - `src/slic3r/Utils/NetworkAgent.hpp`
  - `src/slic3r/Utils/NetworkAgent.cpp`
  - `src/slic3r/Utils/bambu_networking.hpp`
- **Precondizione**: nessun dialog, manager, login, monitor o print workflow deve più usarli.
- **Motivo**: è il backend Bambu principale.

## Fase 6 - Rimuovere streaming, camera, file system remoto e webview device

### Step 31 - Rimuovere `PrinterFileSystem`

- **Target**:
  - `src/slic3r/GUI/Printer/PrinterFileSystem.hpp`
  - `src/slic3r/GUI/Printer/PrinterFileSystem.cpp`

### Step 32 - Rimuovere `BambuTunnel` e `gstbambusrc`

- **Target**:
  - `src/slic3r/GUI/Printer/BambuTunnel.h`
  - `src/slic3r/GUI/Printer/gstbambusrc.h`
  - `src/slic3r/GUI/Printer/gstbambusrc.c`
- **Precondizione**: camera/stream non più disponibili.

### Step 33 - Rimuovere `BambuPlayer` se presente solo per stream Bambu

- **Target**: directory `src/slic3r/GUI/BambuPlayer/`
- **Precondizione**: nessun riferimento residuo in camera o player UI.

### Step 34 - Rimuovere `PrinterWebView` solo se serve solo al device remoto

- **Target**:
  - `src/slic3r/GUI/PrinterWebView.hpp`
  - `src/slic3r/GUI/PrinterWebView.cpp`
- **Attenzione**: verificare che non sia usata per contenuti generici.

### Step 35 - Rimuovere `WebViewDialog` e `Widgets/WebView` solo se non condivisi

- **Target**:
  - `src/slic3r/GUI/WebViewDialog.hpp`
  - `src/slic3r/GUI/WebViewDialog.cpp`
  - `src/slic3r/GUI/Widgets/WebView.hpp`
  - `src/slic3r/GUI/Widgets/WebView.cpp`
- **Regola**: se sono usati anche fuori dai flow Bambu, non rimuoverli.

## Fase 7 - Rimuovere integrazioni app-wide

### Step 36 - Pulire `MainFrame` dai flow Bambu/AMS

- **File**:
  - `src/slic3r/GUI/MainFrame.hpp`
  - `src/slic3r/GUI/MainFrame.cpp`
- **Da rimuovere**:
  - menu, comandi, tab e azioni per device/monitor/send/bind
  - include dei dialog rimossi
- **Da mantenere**:
  - `BBLTopbar`
  - qualsiasi flusso About

### Step 37 - Pulire `Plater` dai flow di invio e AMS

- **File**:
  - `src/slic3r/GUI/Plater.hpp`
  - `src/slic3r/GUI/Plater.cpp`
- **Da rimuovere**:
  - aperture di `SendToPrinter`
  - gestione mappature AMS
  - refresh AMS di sidebar o preset se davvero legati ai device Bambu

### Step 38 - Pulire `Tab` e preset UI dalle opzioni Bambu/AMS

- **File**:
  - `src/slic3r/GUI/Tab.hpp`
  - `src/slic3r/GUI/Tab.cpp`
  - eventuali file preset UI correlati
- **Da rimuovere**:
  - pagine AMS
  - opzioni visibili solo per Bambu/AMS
  - collegamenti a calibrazione Bambu se si decide di eliminarla

### Step 39 - Pulire `GUI_App` dai membri e callback Bambu

- **File**:
  - `src/slic3r/GUI/GUI_App.hpp`
  - `src/slic3r/GUI/GUI_App.cpp`
- **Da rimuovere solo alla fine**:
  - `m_agent`
  - `m_device_manager`
  - `m_user_manager`
  - `m_http_server`
  - callback di subscribe, bind, machine alive, local connect, cloud sync
- **Da mantenere**:
  - top bar generale
  - funzioni non legate ai device Bambu

### Step 40 - Pulire `Event.hpp` dagli eventi AMS/BBL non più usati

- **Target**: `src/slic3r/GUI/Event.hpp`
- **Metodo**: rimuovere solo eventi rimasti orfani dopo i passi precedenti.

## Fase 8 - Rimuovere configurazioni e formati specifici

### Step 41 - Rimuovere opzioni AMS/BBL da `PrintConfig`

- **File**:
  - `src/libslic3r/PrintConfig.hpp`
  - `src/libslic3r/PrintConfig.cpp`
- **Esempi da verificare**:
  - `ams_mapping`
  - `ams_mapping_info`
  - `task_use_ams`
  - opzioni specifiche cloud/device Bambu

### Step 42 - Rimuovere config Bambu/AMS da `AppConfig`

- **File**:
  - `src/libslic3r/AppConfig.hpp`
  - `src/libslic3r/AppConfig.cpp`
- **Da pulire**:
  - preferenze cloud Bambu
  - device selezionato
  - cache AMS o device se presenti

### Step 43 - Rimuovere `bbs_3mf` solo se non è richiesto per compatibilità file

- **Target**:
  - `src/libslic3r/Format/bbs_3mf.hpp`
  - `src/libslic3r/Format/bbs_3mf.cpp`
- **Regola**: farlo solo se GingerSlicer non deve più leggere/scrivere il formato Bambu Studio.

## Fase 9 - Calibrazione Bambu: opzionale e separata

### Step 44 - Decidere se la calibrazione Bambu è nel perimetro

- **File candidati**:
  - `CalibrationPanel*`
  - `CalibrationWizard*`
  - `ExtrusionCalibration*`
  - `CalibUtils*`
  - `libslic3r/calib.*`
- **Regola**:
  - rimuoverli solo se sono davvero una feature esclusiva delle stampanti Bambu
- **Nota importante**: alcune pagine di calibrazione usano `BBLStatusBarSend`; questo è un altro motivo per non rimuovere subito quella classe.

## Fase 10 - Pulizia build e risorse

### Step 45 - Pulire `src/slic3r/CMakeLists.txt`

- **Da rimuovere**:
  - sorgenti dei file eliminati nei passi precedenti
  - eventuale `add_subdirectory(DeviceTab)` se non serve più
  - dipendenze GStreamer usate solo per `bambu:///` o streaming Bambu
- **Da mantenere**:
  - sorgenti di componenti condivisi come `BBLTopbar`

### Step 46 - Pulire risorse e preset specifici Bambu

- **Controllare**:
  - `resources/`
  - preset macchina/materiale
  - eventuali asset web o immagini device-specifiche
- **Regola**: rimuovere solo risorse sicuramente dedicate a Bambu/AMS.

## Regole decisionali per i casi ambigui

Se un file o una classe ha nome `BBL*`, applicare questa regola:

- **se rappresenta branding storico ma funzione generale, mantenere**
- **se implementa un workflow esclusivo di login, bind, monitor, cloud, send, camera o AMS, rimuovere**

Esempi pratici:

- **Mantenere**: `BBLTopbar`
- **Probabilmente mantenere fino a prova contraria**: `BBLStatusBar`, `BBLStatusBarSend`
- **Rimuovere**: `WebUserLoginDialog`, `BindDialog`, `SelectMachine`, `SendToPrinter`, `AMSSetting`, `AMSControl`

## Ordine consigliato di esecuzione

1. `WebUserLoginDialog`
2. `BindDialog`
3. `SelectMachine`
4. `SelectMachinePop`
5. `SendToPrinter`
6. `SendMultiMachinePage`
7. multi-device pages
8. `AmsMappingPopup`
9. `AMSSetting`
10. `AMSMaterialsSetting`
11. `AMSControl`
12. `AMSItem`
13. `AmsWidgets`
14. `uiAmsHumidityPopup`
15. `StatusPanel`
16. `UpgradePanel`
17. `HMSPanel`
18. `MediaFilePanel`
19. `CameraPopup`
20. `Monitor*`
21. `BindJob`
22. `SendJob`
23. `PrintJob` se davvero Bambu-only
24. `BBLStatusBarBind`
25. `BBLStatusBarSend` solo se rimasta orfana
26. `BBLStatusBar` solo se rimasta orfana
27. `UserManager`
28. `HttpServer`
29. `DeviceManager`
30. `NetworkAgent` e `bambu_networking.hpp`
31. `PrinterFileSystem`
32. `BambuTunnel` e `gstbambusrc`
33. `BambuPlayer`
34. `PrinterWebView` se non condivisa
35. `WebViewDialog` e `Widgets/WebView` se non condivisi
36. pulizia `MainFrame`
37. pulizia `Plater`
38. pulizia `Tab`
39. pulizia `GUI_App`
40. pulizia `Event.hpp`
41. pulizia `PrintConfig`
42. pulizia `AppConfig`
43. `bbs_3mf` se non più richiesto
44. calibrazione Bambu solo se confermata nel perimetro
45. pulizia `CMakeLists.txt`
46. pulizia risorse e preset

## Verifica dopo ogni step

- compilare il progetto
- cercare simboli orfani del file appena rimosso
- rimuovere include morti
- rimuovere eventi morti
- rimuovere sorgenti dal CMake solo quando il file è davvero sparito
- evitare refactor non richiesti

## Esito atteso finale

Alla fine devono sparire:

- login cloud BambuLab
- bind/unbind device BambuLab
- invio stampa a device BambuLab
- monitor remoto BambuLab
- streaming/camera/file browser BambuLab
- tutta la UI AMS
- configurazioni AMS/BBL non più usate

Alla fine devono restare:

- slicing locale
- export normali
- UI generale condivisa
- `BBLTopbar`
- tutto l'`About`
