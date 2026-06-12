---

# 🟢 ASSOLUTAMENTE DA MANTENERE (Core Architecture - 31 file)

## Rendering & Visualization (9 files)
- **3D Scene**: GLVolume, rendering foundation
- **3DBed**: Bed plane 3D rendering
- **Camera**: 3D view projection system
- **GLModel**: GPU geometry abstraction
- **GLShader**: GLSL shader compilation
- **GLShadersManager**: Shader cache
- **GLTexture**: GPU texture management
- **OpenGLManager**: GL context lifecycle
- **GCodeViewer**: G-code 3D visualization

## Gizmo System (4 files)
- **GLGizmoBase**: Gizmo interaction framework
- **GLGizmosManager**: Gizmo orchestration
- **GLGizmosCommon**: Common data pool
- **GLGizmoPainterBase**: Paint-on gizmo base

## GUI Foundation (8 files)
- **GLCanvas3D**: Main 3D editing canvas
- **Tab**: Print/Filament/Printer settings panel
- **Field**: Config option UI widgets
- **Plater**: Main editing workspace
- **Selection**: 3D object selection system
- **PartPlate**: Single plate management
- **GUI_App**: Application lifecycle
- **MainFrame**: Main window

## Configuration & State (6 files)
- **PrintConfig**: Parameter definitions (core)
- **Config/Snapshot**: Configuration versioning
- **Config/Version**: Bundle version schema
- **UndoRedo**: Undo/redo system
- **ProjectDirtyStateManager**: Change tracking
- **OptionsGroup**: Config UI grouping

## Background Processing (4 files)
- **BackgroundSlicingProcess**: Async slicing thread
- **Worker**: Job queue framework
- **Job**: Job base abstraction
- **ProgressIndicator**: Progress tracking interface

---

# 🟡 DA DECIDERE - Low to Medium Priority (67 file)

## Gizmo Specializations (27 files)
Geometry editing tools - usefulness varia in base a use case:
- GLGizmoMove, GLGizmoScale, GLGizmoRotate: Basic 3D transforms (KEEP - essential)
- GLGizmoCut, GLGizmoMeshBoolean: Boolean CAD operations (MEDIUM - specializzato)
- GLGizmoMmuSegmentation, GLGizmoSeam, GLGizmoFuzzySkin: Painting tools (MEDIUM - print quality)
- GLGizmoEmboss, GLGizmoText, GLGizmoSVG: 3D text/design (MEDIUM - feature-specific)
- GLGizmoAdvancedCut, GLGizmoMeasure: Advanced measurement/cutting (MEDIUM - CAD-grade)
- GLGizmoFdmSupports, GLGizmoBrimEars: Paint-on features (MEDIUM)
- GLGizmoFlatten: Auto-orient on curved surfaces (LOW - rare workflow)
- GLGizmoSimplify: Mesh optimization (MEDIUM - useful)
- GLGizmoAssembly, GLGizmoHollow, GLGizmoSlaSupports, GLGizmoFaceDetector: SLA-specific (LOW - SLA niche)

## UI Widgets & Panels (25 files)
Custom wxWidgets extensions - decide per platform standardization:
- Button, CheckBox, TextInput, Label: Basic widgets (KEEP - everywhere)
- ComboBox, DropDown, RadioBox: Selection widgets (KEEP - everywhere)
- ProgressBar, ProgressDialog: Progress UI (KEEP - important)
- Advanced: AxisCtrlButton, StepCtrl, ImageSwitchButton, SwitchButton, TabCtrl (MEDIUM - specialized UI)
- Container/Layout: StaticBox, ScrolledWindow, LabeledStaticBox, PopupWindow (MEDIUM - reusable)
- Niche: RoundedRectangle, TempInput, SpinInput, StaticLine (LOW - very specific)

## Background Jobs (11 files)
Async work queues - usefulness by feature:
- BoostThreadWorker, PlaterWorker, ThreadSafeQueue: Threading infrastructure (KEEP)
- ArrangeJob, FillBedJob, OrientJob: Core arrange/orient (KEEP - essential)
- EmbossJob, CreateFontNameImageJob, CreateFontStyleImagesJob: Text/emboss (MEDIUM)
- SLAImportJob, RotoptimizeJob: SLA-specific (LOW)
- UpgradeNetworkJob: Plugin auto-update (LOW - rare)

## GUI Utilities & Helpers (15 files)
Infrastructure and dialog utilities:
- BitmapCache, ScalableBitmap, GUI_Utils: DPI scaling (KEEP - ubiquitous)
- Search: Option search system (MEDIUM - useful but not critical)
- RaycastManager: Mouse picking infrastructure (KEEP - essential for gizmos)
- UnsavedChangesDialog, MsgDialog: Message dialogs (KEEP)
- NotificationManager: Toast notifications (KEEP - feedback UX)
- SavePresetDialog, CreatePresetsDialog: Preset management (MEDIUM)
- ObjColorDialog, OAuthDialog: Feature-specific (MEDIUM)
- SysInfoDialog, NetworkTestDialog: Diagnostics (LOW - support tools)
- PresetComboBoxes, PresetHints: Preset UI (MEDIUM)

## Specialized UI Dialogs (10 files)
Feature-specific panels and dialogs - utility varia:
- BBLStatusBar, BBLTopbar, NotificationManager: BBS-specific UI (MEDIUM - nice but replaceable)
- GCodeViewer: G-code visualization (KEEP - essential preview)
- Auxiliary, AuxiliaryDialog, GUI_AuxiliaryList: Project file attachments (LOW - BBS feature)
- PhysicalPrinterDialog, PrintHostDialogs: PrintHost UI (MEDIUM - network printing)
- PlateSettingsDialog, GUI_ObjectTable: Multi-plate features (MEDIUM - BBS feature)
- BedShapeDialog, WipeTowerDialog, calib_dlg: Printer configuration (MEDIUM - specific)
- DailyTips, HintNotification: Onboarding (LOW - nice-to-have)

## Print Host & Network (10 files)
Cloud printer integrations - strong deprecation candidates:
- **PrintHost (base)**: Abstract framework (KEEP - architecture)
- **OctoPrint, PrusaLink**: Industry standard (KEEP - OctoPrint huge ecosystem)
- AstroBox, FlashAir, Bonjour: Legacy/wireless tech (LOW - obsolete)
- CrealityPrint, MKS, Flashforge, Elegoo: Vendor-specific (MEDIUM - vendor loyalty)
- Duet, Repetier, ESP3D, SimplyPrint, Obico: Niche hosts (LOW - specialized)

## Utils - File & System (10 files)
System integration utilities:
- Serial, SerialMessage, TCPConsole: Hardware communication (KEEP - printer control)
- Http, WebSocketClient: Network clients (KEEP - essential)
- FileHelp, Process, InstanceCheck: System utilities (KEEP)
- RemovableDriveManager: USB export (MEDIUM - useful)
- FixModelByWin10, FontConfigHelp, RetinaHelper: Platform-specific (LOW - if cross-platform possible)
- PresetUpdater: Profile OTA update (MEDIUM - feature-specific)

## Utils - Minor Helpers (6 files)
Miscellaneous small utilities:
- ColorSpaceConvert: Color math (LOW - very specific)
- EmbossStyleManager: Font caching (MEDIUM - if keeping emboss)
- json_diff: Settings compression (LOW - internal)
- minilzo_extension: LZO compression (LOW - internal)
- ASCIIFolding: Firmware name legacy support (LOW - legacy)
- Profile (Shiny profiling): Debug only (LOW - dev-only)

---

# 🔴 DA PULIRE - Candidates for Removal (20 file)

## BBS/Bambu-Specific Features (7 files)
Proprietary Bambu Studio extensions - consider if not essential:
- **BBLStatusBarSend**: Proprietary send UI (BBS only)
- **ModelMall**: Model marketplace web UI (BBS only)
- **Project, ProjectDirtyStateManager**: Project file attachment (BBS only)
- **Downloader, DownloaderFileGet, DownloadProgressDialog**: Model downloader (BBS only)
- **PrinterWebView**: Web printer interface panel (BBS probably)

## SLA-Only Features (4 files)
Stereolithography support - if SLA removed:
- GLGizmoHollow
- GLGizmoSlaSupports
- RotoptimizeJob
- SLAImportDialog/SLAImportJob (borderline - 2 files)

## Desktop Environment Integration (2 files)
Platform-specific integration - low maintenance value:
- **DesktopIntegrationDialog**: Linux .desktop files (Linux only)
- **Mouse3DController**: 3DConnexion device support (niche/expensive hardware)

## Deprecated/Unused Components (7 files)
Almost-empty or obsolete files:
- **ConfigWizard**: Setup wizard removed, only enum stubs left
- **WebUpdatePlugin**: Completely empty (1 line)
- **UserNotification**: Completely empty stub
- **SingleChoiceDialog**: Minimalist not reused
- **WebViewDialog**: Browser integration for marketplace (BBS only)
- **KBShortcutsDialog**: Keyboard shortcuts help (nice but low priority)
- **PrivacyUpdateDialog**: Privacy/update notifications (could consolidate)

## Legacy/Niche Utilities (3 files)
Very specific legacy support:
- **InstanceID**: Unique ID generation (probably unused)
- **ProfileDescription**: 28 hardcoded preset descriptions (i18n could handle this)
- **Bonjour**: mDNS lookup (could use system API directly)

---

## Summary Statistics

| Category | Count | Action |
|----------|-------|--------|
| **Keep (Core)** | 31 | Essential - no removal |
| **Decide** | 67 | Case-by-case evaluation |
| **Clean** | 20 | Strong removal candidates |
| **TOTAL** | **118** | |

### Cleanup Strategy
1. **Phase 1**: Remove 20 low-priority BBS/SLA/deprecated (easily justified)
2. **Phase 2**: Audit 67 "decide" files for cross-module dependencies before cleanup
3. **Phase 3**: Consolidate remaining 31 core files, possible refactoring simplification

---

# 📋 Original File Descriptions (Reference)

# Utils/ASCIIFolding
Utilità fold UTF-8 accented caratteri a ASCII: fold_utf8_to_ascii() legacy firmware filename compatibility, fold_to_ascii(wchar_t) singolo carattere. Feature accented character stripping.

# Utils/AstroBox
PrintHost AstroBox: inherits PrintHost, HTTP API key auth, test/upload/start print, auto-discovery enabled. Feature AstroBox cloud printer integration.

# Utils/Bonjour
mDNS service discovery Bonjour: struct BonjourReply (ip/port/service_name/txt_data), class Bonjour async lookup/resolve con callback fn. Feature printer auto-discovery network.

# Utils/ColorSpaceConvert
Color space conversione: RGB↔Lab↔XYZ pipeline, RGB↔HSV, RGB→YUV, DeltaE76/94/00 color difference metriche, wxColour converters. Feature color math utilities.

# Utils/CrealityPrint
PrintHost CrealityPrint: cloud API, SSL config, web UI URL, chunked upload, start print action. Feature Creality cloud printer integration.

# Utils/Duet
PrintHost Duet: RRF/DSF connection modes, password auth, simulation mode start, timestamp URL. Feature Duet 3D printer integration.

# Utils/ElegooLink
PrintHost ElegooLink (inherits OctoPrint): WebSocket client per print control, chunked upload con MD5, bed leveling/filament/timelapse opzioni.

# Utils/EmbossStyleManager
Emboss style manager: font caching (imgui + wxFont), style list storage/load config, add/rename/erase/swap style, GPU font release. Core emboss text styling.

# Utils/ESP3D
PrintHost ESP3D: TCP console integration, command format utility, file upload, start print action, error code parsing. Feature ESP3D firmware integration.

# Utils/FileHelp
File utility semplice: is_file_too_large(file_path, try_ok), slash_to_back_slash conversione path Windows. Minimale file helper.

# Utils/FixModelByWin10
Windows 10 SDK model fixing wrapper: fix_model_by_win10_sdk_gui() thin wrapper ProgressDialog, conditional compile HAS_WIN10SDK. Platform-specific model repair.

# Utils/FlashAir
PrintHost FlashAir: SD card file upload via HTTP, timestamp URL generation, simple host-based. Feature FlashAir wireless SD card integration.

# Utils/Flashforge
PrintHost Flashforge: serial command protocol (SerialMessage), Klipper vs legacy firmware support, buffer size config, device info/status commands. Feature Flashforge printer serial control.

# Utils/FontConfigHelp
Linux font config utility: get_font_path(wxFont) fontconfig library converter wxFont→file path. Linux-only, inspired by wxpdfdoc.

# Utils/HexFile
Hex file metadata: struct HexFile (path, device enum MK2/MK3/MM_CONTROL/CW1/generic, model_id). Feature Prusa firmware HEX file identification.

# Utils/Http
HTTP client wrapper: static factory get/post/put/del/patch, Progress callback (dltotal/dlnow/ultotal/ulnow), error handling enum, extra headers global. Core HTTP communication.

# Utils/InstanceID
Instance ID generator: ensure(AppConfig) canonical IID generation, reset_cache_for_tests(). Minimale utility per unique instance identification.

# Utils/json_diff
JSON differencing: load_compatible_settings(type, version), all2diff/diff2all transformation base, decode error count tracking. Feature settings diff compression.

# Utils/minilzo_extension
LZO compression wrapper: lzo_compress/lzo_decompress(in, in_len, out, out_len) thin wrapper minilzo library. Feature compression utility.

# Utils/MKS
PrintHost MKS: TCP console integration, file upload con URL construction, device status/temp/print command, error code parsing. Feature MKS printer integration.

# Utils/Obico
PrintHost Obico (inherits PrintHost): cloud service, OAuth login, get_login_url(auth_url), API key config, test/upload, get_printers lista. Feature Obico cloud integration.

# Utils/OctoPrint
PrintHost OctoPrint: HTTP API key auth, auto-discovery, start print action, cafile SSL support. Plus subclass `PrusaLink` (HTTP Digest auth, PUT/POST dual-mode upload, storage selection).

# Utils/PresetUpdater
Preset updater OTA: sync(http_url, language) background config fetch, slic3r_update_notify(), config_update enum result (noop/update/reject/notification), install_bundles_rsrc(list). Feature profile OTA update.

# Utils/PrintHost
Classe base astratta PrintHost: enum PostUploadAction (none/start/simulate/queue), struct PrintHostUpload (path/storage/post_action), test/upload virtual methods, factory get_print_host(config). Core print host framework.

# Utils/Process
Process launcher semplice: start_new_slicer(path, single_instance), start_new_gcodeviewer(path), start_new_gcodeviewer_open_file(). Utility processi GUI separati.

# Utils/Profile
Wrapper profiling Shiny intrusive: SLIC3R_GUI_PROFILE_FUNC/BLOCK/UPDATE/OUTPUT macros (disabled per default senza SLIC3R_PROFILE_GUI). Minimale profiling hook.

# Utils/ProfileDescription
Namespace ProfileDescrption: array 28 stringhe localizzate PROFILE_DESCRIPTION_* descrizioni layer height profili BBS (layer height vs quality vs time tradeoff).

# Utils/RaycastManager
Raycasting manager 3D: struct Hit (tr_key, squared_distance, position/normal), AABBMesh per ray picking, ISkip interfaccia filtraggio, actualize(object/instance). Core mouse picking.

# Utils/Repetier
PrintHost Repetier: server multi-printer/group support, API key auth, get_groups/get_printers lista. Feature Repetier server integration multi-device.

# Utils/RetinaHelper
macOS Retina display support: get_use_retina(), get_scale_factor(), platform-specific wrapper opaco (pimpl pattern). Minimale DPI scaling helper.

# Utils/Serial
Serial port wrapper: struct SerialPortInfo (port/vendor_id/product_id/friendly_name/is_printer), scan_serial_ports(), class Serial thin boost::asio wrapper. Feature serial port utilities.

# Utils/SerialMessage
Struct SerialMessage minimale: message std::string + messageType (Command/Data enum). Semplice comando/dato wrapper.

# Utils/SerialMessageType
Enum SerialMessageType: Command, Data. Minimale type tag per messaggi seriali.

# Utils/SimplyPrint
PrintHost SimplyPrint: cloud OAuth login credential storage, chunked upload >100MB con MD5, temp upload API, API retry on token refresh, QueuePrint post-action. Feature SimplyPrint integration.

# Utils/TCPConsole
TCP telnet-like console: boost::asio socket/resolver, enqueue_cmd(SerialMessage) queue, run_queue(), timeout configurabile, line delimiter/done string. Core TCP command interface.

# Utils/UndoRedo
Undo/Redo snapshot management: enum SnapshotType (Action/GizmoAction/Selection/ProjectSeparator/EnteringGizmo/LeavingGizmo*), struct SnapshotData (tipo, flags, printer_technology), Snapshot timestamp. Core undo-redo system.

# Utils/WebSocketClient
WebSocket client wrapper: boost::beast websocket, connect(host, port, path), send/receive(timeout), User-Agent decorator, try-catch error handling. Feature WebSocket communication.

# Utils/WxFontUtils
wxFont utilities: can_load, create_font_file(wxFont)→FontFile, serialize/deserialize wxFont per persistenza, set_italic/set_bold con Emboss integration. Feature wxFont helpers emboss.
---
# Config/Snapshot
Gestisce snapshot della configurazione utente (preset print/filament/printer/vendor). Classe `Snapshot` salva/carica ini, confronta directory attiva, esporta selezioni. `SnapshotDB` indicizza tutti gli snapshot su disco. Usato dal sistema di upgrade/rollback configurazione.

# Config/Version
Struttura `Version` per versioni bundle di configurazione (config_version, min/max slic3r_version). Classe `Index` carica/salva indice versioni vendor scaricato da internet. Usato dal sistema OTA di aggiornamento profili.

# GUI/2DBed
Widget wxPanel `Bed_2D` che disegna il piano di stampa 2D (usato in BedShapeDialog). Converte coordinate, genera griglia, disegna sagoma del piatto. Utile e attivo.

# GUI/3DBed
Classe `Bed3D` per rendering OpenGL del piano di stampa 3D nel canvas principale. Gestisce texture, griglia, modello STL del piatto, colori assi. Core del rendering del bed, necessario.

# GUI/3DScene
Header principale della scena 3D OpenGL: `GLVolume`, `GLVolumeCollection`, macro `glsafe`/`glcheck` per debug GL, helper colori estrusori. Base di tutto il rendering 3D.

# GUI/AboutDialog
Dialog "About": logo, testo versione, link. `CopyrightsDialog` mostra librerie terze con copyright/link HTML. Utile per UI ma non critico per slicing.

# GUI/Auxiliary
Pannello "Auxiliary Files" (file allegati al progetto 3MF): lista file, upload/download, anteprima immagini. Usa wxDataViewCtrl, integra con ProjectTask. Feature BBS/Bambu.

# GUI/AuxiliaryDataViewModel
ViewModel wxDataViewModel per il pannello Auxiliary Files. Gestisce nodi ad albero (cartelle/file) per wxDataViewCtrl. Dipendente da Auxiliary.hpp.

# GUI/AuxiliaryDialog
Dialog wrapper per `AuxiliaryList`: finestra modale che ospita il pannello lista file ausiliari. Minimalista, dipende da GUI_Utils.

# GUI/BackgroundSlicingProcess
Thread background per slicing asincrono. Gestisce stati (idle/slicing/cancellatoin/error), eventi wxEvent al completamento, esportazione G-code/SLA, invio a print host. Core del processo di slicing, indispensabile.

# GUI/BBLStatusBar
Barra di stato inferiore (wxPanel): progress bar, testo stato, bottone annulla, info oggetti/slicing. Implementa `ProgressIndicator`. Sostituisce la wxStatusBar standard. Necessario.

# GUI/BBLStatusBarSend
Variante della status bar per il pannello di invio stampa: progress bar con blocchi colorati, link errore espandibile, bottone annulla. Specifica per flusso send-to-printer BBS.

# GUI/BBLTopbar
Barra titolo/menu custom (wxAuiToolBar): file menu, undo/redo, salva, pubblica, calibrazione, finestra draggabile. Sostituisce la titlebar nativa su Windows. Necessario per UI BBS/Bambu.

# GUI/BedShapeDialog
Dialog configurazione forma piatto: rettangolare, circolare, custom. Panel `BedShapePanel` con preview 2D e opzioni dimensione/origine. Aperto da Print Settings→Bed Shape.

# GUI/BitmapCache
Cache globale bitmap wxWidgets: carica PNG/SVG da resources/icons, scala per Retina, converte in grayscale, crea bitmap colore solido. Componente infrastrutturale GUI, molto usato.

# GUI/BitmapComboBox
`BitmapComboBox` estende wxBitmapComboBox con fix per macOS (scaling Retina) e Windows (custom draw). Usato nelle combo preset sidebar/tab.

# GUI/BonjourDialog
Dialog ricerca stampanti via Bonjour/mDNS: lista risultati live, selezione IP. Anche `IPListDialog` per scelta IP multipli. Usato in configurazione PrintHost.

# GUI/calib_dlg
Dialog di calibrazione: PA (Pressure Advance), temperatura, flusso, VFA, max volumetric speed. Genera print job di calibrazione. Feature di calibrazione automatica OrcaSlicer.

# GUI/Camera
Struttura `Camera` per la vista 3D OpenGL: tipo (Ortho/Perspective), zoom, target, matrice view/projection, frustrum. Gestisce tutti i parametri di proiezione. Core del rendering, indispensabile.

# GUI/CameraUtils
Utility statiche per la camera: proiezione 3D→2D, ray casting da schermo, hull 2D di GLVolume, posizione Z=0 sotto mouse. Supporto a picking e gizmos.

# GUI/CloneDialog
Dialog "Clona oggetti": spinner conteggio copie, checkbox arrange automatico, progress bar. Lancia clone+arrange in background. Funzione utile del Plater.

# GUI/ConfigExceptions
Header minimale: eccezioni `ConfigError` e `ConfigGUITypeError` per errori di tipo nelle opzioni GUI. Solo 16 righe, usato come base eccezioni config.

# GUI/ConfigManipulation
Classe centrale per validazione e toggle visibilità campi UI al cambio config. Gestisce cross-field dependencies (toggle_print_fff_options, update_print_fff_config, ecc.). Indispensabile per Tab.cpp.

# GUI/ConfigWizard
**Setup Wizard rimosso.** Rimangono solo enum `RunReason` e `StartPage` per compatibilità con codice esistente. Candidato eliminazione (o riduzione a solo enum in altro header).

# GUI/CreatePresetsDialog
Dialog per creazione preset filamento e stampante: scelta vendor/tipo/seriale, selezione stampanti compatibili, generazione nome preset. Feature gestione preset utente.

# GUI/DailyTips
Pannello ImGui "Daily Tips": mostra suggerimenti giornalieri da hint database con navigazione pagine, espansione/collasso, fade. Anche `DailyTipsWindow` come wrapper.

# GUI/DesktopIntegrationDialog
**Solo Linux.** Dialog integrazione desktop: crea/rimuove file .desktop e icone per GingerSlicer e GcodeViewer. Gestisce registrazione URL scheme per downloader.

# GUI/Downloader
Sistema download file via URL: classe `Download` (stato, pausa, annulla) e `Downloader` (gestione lista download, eventi wxEvtHandler). Usato per download modelli da web.

# GUI/DownloaderFileGet
Layer HTTP basso livello per download file: `FileGet` con PIMPL, usa Utils/Http. Emette eventi wxEvent (completamento, progresso, errore, pausa). Dipendenza di Downloader.

# GUI/DownloadProgressDialog
Dialog progresso download/aggiornamento network plugin: progress bar, note di rilascio, stato installazione. Lancia `UpgradeNetworkJob`. Feature aggiornamento plugin rete BBS.

# GUI/DragCanvas
Canvas wxPanel per drag&drop di swatches colore (ordine estrusori). `DragShape` rappresenta un colore trascinabile. Usato per riordinare filamenti multi-materiale.

# GUI/EditGCodeDialog
Dialog editor G-code custom con lista variabili placeholder (stati slicing, statistiche, dimensioni, temperature). Testo editor + ricerca variabile + inserimento. Usato da Tab custom gcode fields.

# GUI/Event
Header template wxEvent riutilizzabili: `SimpleEvent`, `IntEvent`, `ArrayEvent<T,N>`, `Event<T>`. Infrastruttura eventi tipizzati per comunicazione tra componenti GUI.

# GUI/ExtraRenderers
Renderer custom per wxDataViewCtrl: `DataViewBitmapText` (testo+icona), `BitmapTextRenderer` (editing inline), `BitmapChoiceRenderer` (combo con icona). Usato in ObjectList e altri DataView.

# GUI/Field
Classi widget per opzioni config GUI: `Field` (base), `TextCtrl`, `CheckBox`, `SpinCtrl`, `Choice`, `ColourPicker`, ecc. Gestisce undo/redo visuale, tooltip, enable/disable. Cuore dei Tab settings, indispensabile.

# GUI/FileArchiveDialog
Dialog visualizzazione contenuto archivi ZIP/3MF: tree view con toggle per includere/escludere file, anteprima struttura. Usato per import/export selettivo file archivio.

# GUI/format
Wrapper `format_wxstr()` attorno a `boost::format` per produrre `wxString` da stringhe UTF8 miste. Usato pervasivamente in tutta la GUI per messaggi localizzati.

# GUI/GCodeViewer
Viewer G-code 3D OpenGL: parsing GCodeProcessor, buffer vertex multi-tipo (extrusion/travel/wipe/retractions), colorazione per ruolo/temperatura/velocità/materiale, slider layer/move. Core della preview G-code, >900 righe header.

# GUI/GLCanvas3D
Canvas OpenGL principale (~1272 righe header): gestisce rendering scena 3D, input mouse/tastiera, selezione, gizmos, toolbar GL, bed, GCodeViewer. Nucleo dell'interazione 3D.

# GUI/GLModel
Classe `GLModel`: geometria OpenGL (vertex buffer, index buffer, layout P2/P3/P3N3/ecc), upload GPU, render. Helper factory per primitive (arrow, sphere, cylinder, ecc). Base di tutto il rendering 3D.

# GUI/GLSelectionRectangle
Rettangolo di selezione rubber-band nel canvas 3D: stati off/select/deselect, contiene punti 3D nel rettangolo 2D. Usato per selezione multipla oggetti.

# GUI/GLShader
`GLShaderProgram`: compilazione/linking shader GLSL (vertex, fragment, geometry, tessellation, compute), cache uniform/attrib location, set_uniform per tutti i tipi. Infrastruttura shader OpenGL.

# GUI/GLShadersManager
Gestore centralizato shader: `init()` compila tutti gli shader dell'app, `get_shader(name)` li recupera per nome. Piccolo ma fondamentale per il sistema GL.

# GUI/GLTexture
`GLTexture`: carica PNG/SVG su GPU, compressione DXT in thread background, supporto mipmap. Usata per texture bed, icone toolbar GL, testi ImGui.

# GUI/GLToolbar
Toolbar OpenGL su canvas 3D: pulsanti con icone SVG, eventi wxEvent per ogni azione (slice, print, add, delete, arrange, clone, ecc). `GLRadioToolbar` per toggle view 3D/preview/assemble.

# GUI/GUI
Header utility globali GUI: funzioni `show_error`, `show_info`, `change_opt_value`, `disable_screensaver`, prefix shortcut (Ctrl/⌘). Entry point helpers condivisi.

# GUI/GUI_App
Classe principale applicazione `GUI_App` (wxApp): gestisce PresetBundle, Plater, MainFrame, ImGui, OpenGL, aggiornamenti, istanza singola, dark mode. Entry point dell'intera GUI.

# GUI/GUI_AuxiliaryList
`AuxiliaryList` (wxDataViewCtrl): lista file ausiliari con drag&drop, context menu (crea cartella, importa, elimina, rinomina, apri). Usato nel pannello Auxiliary.

# GUI/GUI_Colors
Enum `RenderCol_` e classe `RenderColor` con array `ImVec4 colors[]` per tutti i colori di rendering personalizzabili (sfondo, piatti, modelli, assi, grabbers). Usato da Preferences per tema colori.

# GUI/GUI_Factories
`SettingsFactory`: raggruppa opzioni config per categoria, bitmap icone categorie, opzioni visibili per oggetto/parte. `MenuFactory`: costruisce menu contestuale oggetti con bitmap volumi (solid/part/modifier/negative). Core UI settings.

# GUI/GUI_Geometry
`TransformationType`: enum bitfield per coordinate (World/Instance/Local) + modalità (Absolute/Relative) + gruppo (Joint/Independent). Usato da gizmos per determinare sistema coordinate trasformazioni.

# GUI/GUI_Init
`GUI_InitParams`: struct parametri avvio GUI (argc/argv, preset_substitutions, load_configs, input_files). `GUI_Run()` entry point loop wxWidgets. Thin wrapper avvio.

# GUI/GUI_ObjectLayers
Pannello editing layer ranges oggetto: `LayerRangeEditor` per MinZ/MaxZ/layer height, lista range con pulsanti +/-/delete. `ObjectLayers` gestisce t_layer_config_ranges. Feature per per-object layer editing.

# GUI/GUI_ObjectList
Tree view wxDataViewCtrl oggetti/volumi/parti: `ObjectDataViewModel`, drag&drop, context menu, selezione multipla, toggles visibility/solid/wireframe. Sidebar principale, ~500 righe header. Core UI.

# GUI/GUI_ObjectSettings
Pannello settings per oggetto/part: `OG_Settings` base, `ObjectSettings` wrapper per ConfigOptionsGroup. Con `NEW_OBJECT_SETTING` usa TabPrintModel invece di pannello separato. Feature settings per-oggetto.

# GUI/GUI_ObjectTable
Grid wxGrid per editing tabellare oggetti: `ObjectTablePanel`, `GridCellIconRenderer`, `GridCellTextEditor`. Feature BBS per editing multi-oggetto in tabella (nome, posizione, rotazione, scale). >600 righe header.

# GUI/GUI_ObjectTableSettings
Pannello settings per riga tabella oggetti: `OTG_Settings` base, `ObjectTableSettings` con ConfigOptionsGroup per riga selezionata. Companion di GUI_ObjectTable.

# GUI/GUI_Preview
Pannello preview G-code: `View3D` con GLCanvas3D, GCodeViewer, slider layer, toolbar. Gestisce visualizzazione print sliced. Core della preview.

# GUI/GUI_Utils
Header utility GUI massiccio (~500 righe): DPI scaling, `DPIDialog`, `ScalableBitmap`, `from_dip`, `msw_rescale`, eventi HID/Volume Windows, `copy_file_gui`. Infrastruttura DPI-aware.

# GUI/HintNotification
`HintDatabase`: singleton per suggerimenti giornalieri, caricamento da file, navigazione prev/next/random. `HintData` struct con testo, hyperlink, callback. Usato da DailyTips.

# GUI/HttpServer
Server HTTP locale (localhost:13618) basato su Boost.Beast: `http_headers`, `session` per gestione richieste. Usato per comunicazione locale con browser/estensioni.

# GUI/I18N
Macro localizzazione `_()`, `_L()`, `_CTX()`, `_utf8()`. Implementazione pass-through (inglese solo) con conversione UTF8. Stub per i18n disabilitato.

# GUI/IconManager
Gestore texture icone ImGui: carica SVG, rasterizza in texture atlas, restituisce `Icon` con UV coordinate. Supporta varianti color/white/gray. Usato da ImGui UI.

# GUI/ImGuiWrapper
Wrapper ImGui: inizializza ImGui, gestisce font (CJK support), input wxEvents, rendering su wxGLCanvas. Helper per slider, button, menu con icone. Bridge wxWidgets↔ImGui.

# GUI/IMSlider
Slider ImGui per layer/time G-code: `IMSlider` con texture, tick marks custom, color change detection, label height/time. Usato in GCodeViewer per navigazione layer.

# GUI/IMToolbar
Toolbar ImGui per piatti: `IMToolbarItem` con stati UNSLICED/SLICING/SLICED/FAILED, texture dinamiche. Mostra progress slicing per piatto. Feature BBS multi-plate.

# GUI/InstanceCheck
Controllo istanza singola: `instance_check()` invia argv a istanza esistente (Windows named pipe, macOS lockfile, Linux DBus). `OtherInstanceMessageHandler` riceve messaggi da altre istanze.

# GUI/KBShortcutsDialog
Dialog scorciatoie tastiera: lista completa shortcut raggruppati per categoria, tab pages, ricerca. Mostra tutte le combinazioni tasti dell'applicazione.

# GUI/MainFrame
Finestra principale wxFrame: menu bar, tab panel (Print/Filament/Printer), Plater, Sidebar, BBLTopbar, gestione eventi, salvataggio progetto, export. ~400 righe header. Core UI.

# GUI/MarkdownTip
Popup wxWebView per tooltip Markdown: carica HTML/CSS, script JS, timer auto-hide. Usato per tooltip avanzati con formattazione.

# GUI/MeshUtils
`ClippingPlane`: piano di clipping per sezione mesh (normal+offset). `Raycaster`: ray picking su mesh (AABBMesh). `MeshRaycaster`: ray su TriangleMesh. Usato da gizmos cut/flattening.

# GUI/ModelMall
Dialog web browser per "Model Mall" (marketplace modelli): wxWebView, navigazione back/forward, download modelli. Feature BBS per scaricare modelli online.

# GUI/Mouse3DController
Supporto dispositivi 3DConnexion (SpaceMouse): thread background HID, parametri scale/deadzone, rotazione/pan/zoom camera. Usato per navigazione 3D professionale.

# GUI/MsgDialog
Dialog messaggi customizzabile: bitmap icona, testo HTML, pulsanti multipli, checkbox "non mostrare più", DPI-aware. Usato pervasivamente per notifiche.

# GUI/NetworkTestDialog
Dialog test rete: ping Bing/Orca, test DNS, scarica log, mostra info sistema/versione. Tool diagnostico per problemi connessione BBS.

# GUI/Notebook
Custom wxBookCtrl con tab buttons: `ButtonsListCtrl` con bitmap attive/inattive, DPI scaling. Sostituisce wxNotebook standard per UI BBS.

# GUI/NotificationManager
Gestore notifiche: popup ImGui, toast, progress bar, notifiche export/slicing/update/download. ~955 righe header. Core sistema notifiche.

# GUI/OAuthDialog
Dialog OAuth: esegue `OAuthJob` in background, mostra progress, restituisce `OAuthResult`. Usato per login cloud BBS.

# GUI/ObjColorDialog
Dialog mappatura colori oggetto→estrusore: algoritmo clustering colori, matching automatico, mapping manuale. Feature per multi-material.

# GUI/ObjectDataViewModel
ViewModel wxDataViewModel per ObjectList: nodi Plate/Object/Volume/Instance/Layer/Settings, colonne nome/print/filament/paint, drag&drop. ~542 righe header.

# GUI/OG_CustomCtrl
Controllo custom per OptionsGroup: rendering linee con bitmap mode/blink, DPI scaling, layout custom. Sostituisce wxStaticBox per UI BBS.

# GUI/OpenGLManager
Gestore contesto OpenGL: `GLInfo` (versione, vendor, estensioni), inizializzazione GL, gestione wxGLContext/wxGLCanvas singleton. Infrastruttura GL.

# GUI/OptionsGroup
`Line`: riga opzioni con label/tooltip/undo buttons. `ConfigOptionsGroup`: raggruppa linee, gestisce Field widgets, callback on_change. Core UI settings.

# GUI/ParamsDialog
Dialog wrapper per `ParamsPanel`: modale con window disabler, editing filamento ID. Thin wrapper per dialog parametri.

# GUI/ParamsPanel
Pannello parametri filamento: `TipsDialog` per suggerimenti, editing proprietà filamento. Feature BBS per gestione filamenti.

# GUI/PartPlate
Classe piatto di stampa: gestisce oggetti sul piatto, arrange, slicing, G-code, thumbnail, export. ~865 righe header. Core multi-plate.

# GUI/PhysicalPrinterDialog
Dialog stampante fisica (PrintHost): configurazione host/port/CA/cert, test connessione, logout. Usato per setup stampanti di rete.

# GUI/Plater
Pannello principale editing: gestisce Model, PartPlateList, GLCanvas3D, Selection, Sidebar, undo/redo, slicing, export. ~837 righe header. Nucleo app.

# GUI/PlateSettingsDialog
Dialog impostazioni piatto: `LayerNumberTextInput` per range layer, `OtherLayersSeqPanel` per sequenza stampa layer, `DragCanvas` per ordine filamenti. Feature BBS per multi-plate.

# GUI/Preferences
Dialog preferenze: tab pages per diverse categorie (generale, GUI, slicing, ecc.), DPI scaling, checkbox/radio/combobox. ~150 righe header. Core settings UI.

# GUI/PresetComboBoxes
`PresetComboBox`: combo preset con bitmap, label marker, physical printer support. `PresetWithModificationDialog`: dialog modifica preset. Sidebar preset selector.

# GUI/PresetHints
Utility statiche: `cooling_description`, `maximum_volumetric_flow_description`, `recommended_thin_wall_thickness`, `top_bottom_shell_thickness_explanation`. Genera hint testuali da preset.

# GUI/PrinterCloudAuthDialog
Dialog autenticazione cloud stampante: wxWebView per OAuth, estrae API key da JS response. Usato per login stampanti cloud BBS.

# GUI/PrinterWebView
Pannello wxWebView per interfaccia stampante: carica URL, invia API key, gestisce navigazione/errori. Feature BBS per controllo stampante web.

# GUI/PrintHostDialogs
`PrintHostSendDialog`: dialog upload G-code a print host (filename, group, storage, post-action). `PrintHostQueueDialog`: lista job upload in coda. Core print host UI.

# GUI/PrivacyUpdateDialog
Dialog privacy/update: wxWebView per Markdown release note, pulsanti confirm/cancel, eventi custom. Usato per notifiche privacy e aggiornamenti.

# GUI/ProgressStatusBar
Status bar con progress: wxStatusBar + gauge + cancel button, timer per busy state. Implementa `ProgressIndicator`. Sostituisce BBLStatusBar in alcune UI.

# GUI/Project
Pannello "Project" (task 3MF): lista file progetto, upload/download, anteprima. `ProjectPanel` con wxWebView, integrazione ProjectTask. Feature BBS.

# GUI/ProjectDirtyStateManager
Gestore stato "dirty" progetto: traccia undo/redo stack, preset changes, project config. Determina se salvare prima di chiudere. Core dirty tracking.

# GUI/PublishDialog
Dialog pubblicazione modelli: step slicing/packing/uploading/fill info, progress bar, cancellazione. Feature BBS per pubblicare su marketplace.

# GUI/RammingChart
Chart wxWindow per ramming/wipe tower: grafico volume/tempo, punti draggabili, spline interpolation. Usato in Filament Settings per tuning ramming.

# GUI/RecenterDialog
Dialog "recenter home": suggerisce di centrare oggetto sul piatto, bitmap icona, pulsanti confirm/close. Feature BBS per UX.

# GUI/ReleaseNote
Dialog note di rilascio: wxWebView per Markdown, check secondario nozzle, liveview link, error handling. ~354 righe header. Feature aggiornamenti.

# GUI/RemovableDriveManager
Gestore drive rimovibili (USB): thread background per enumerazione, eventi plug/unplug, funzioni get_path/eject. Supporto OSX callbacks, Windows volume notifications. Usato per export su USB.

# GUI/SavePresetDialog
Dialog salvataggio preset: input nome, validazione, checkbox save-to-project, radio group system/user. Supporta multi-type preset. UI per salvare preset modificati.

# GUI/SceneRaycaster
Sistema raycasting 3D per picking su mesh: SceneRaycasterItem per oggetti con trasformazione, SceneRaycaster con categorie Bed/Volume/Gizmo. HitResult con posizione/normal. Core per selezione 3D.

# GUI/Search
Sistema ricerca opzioni: OptionsSearcher con fuzzy matching, SearchDialog popup con scrolled window, SearchObjectDialog per oggetti. Supporta inglese/cinese, marked labels per highlight. Core UI search.

# GUI/Selection
Classe selezione oggetti 3D: gestisce GLVolumePtrs, mode Volume/Instance, bounding boxes, trasformazioni (translate/rotate/scale/mirror), clipboard, rendering hints. ~460 righe header. Core selezione.

# GUI/SendSystemInfoDialog
Funzione show_send_system_info_dialog_if_needed() per mostrare dialog invio info sistema se necessario. Thin wrapper per telemetria.

# GUI/SingleChoiceDialog
Dialog scelta singola: ComboBox con opzioni, messaggio, caption. Ritorna indice selezionato. UI semplice per selezione da lista.

# GUI/SlicingProgressNotification
Notifica progress slicing: stati NO_SLICING/BEGAN/PROGRESS/CANCELLED/COMPLETED, progress bar, cancel button, DailyTipsPanel integrato. Render ImGui custom. Core slicing feedback.

# GUI/StepMeshDialog
Dialog import STEP: input linear/angle deflection, checkbox split compound, preview numero facce mesh. Thread background per conversione STEP→mesh. Feature import CAD.

# GUI/SurfaceDrag
Drag&drop volume su superficie mesh: SurfaceDrag struct con offset mouse, trasformazioni world/instance, RaycastManager. Funzioni on_mouse_surface_drag, face_selected_volume_to_camera. Usato da emboss gizmo.

# GUI/SysInfoDialog
Dialog info sistema: wxHtmlWindow per OpenGL info e system info, logo bitmap, pulsante copy to clipboard. Mostra versione, OS, driver GL. Tool diagnostico.

# GUI/Tab
Tab pannello parametri (Print/Filament/Printer): Page con OptionGroups, TabPrint/TabFilament/TabPrinter derivati, gestione preset, toggle opzioni, build dinamico pagine. ~676 righe header. Core settings UI.

# GUI/TabButton
Bottone custom per tab: StaticBox con icona ScalableBitmap, testo, StateColor per text/border/bg, supporto new tag, DPI scaling. Usato da Tabbook.

# GUI/Tabbook
Notebook custom con TabButtonsListCtrl: sostituisce wxNotebook standard, effetti show/hide, DPI scaling, side tools support. Usato per UI BBS multi-tab.

# GUI/TextLines
Model per linee testo emboss: TextLinesModel con GLModel per rendering linee, init da Transform3d/ModelVolumePtrs/StyleManager, calc_line_height_in_mm. Visualizzazione allineamento verticale testo.

# GUI/TickCode
Gestore tick codici G-code (color change, pause, custom): TickCode struct con tick/type/extruder/color, TickCodeInfo con set ticks, add/edit/erase, mode switching. Usato da IMSlider.

# GUI/UnsavedChangesDialog
Dialog diff preset: DiffViewCtrl con ModelNode tree, DiffPresetDialog per confronto preset left/right, UnsavedChangesDialog per modifiche non salvate. ~475 righe header. Core diff UI.

# GUI/UpdateDialogs
Dialog aggiornamenti: MsgUpdateSlic3r (version check), MsgUpdateConfig (config updates), MsgUpdateForced (forced updates), MsgDataIncompatible, MsgNoUpdates. Gestione aggiornamenti preset/app.

# GUI/UserNotification
Enum UserNotificationStyle (NORMAL/WARNING_CONFIRM), classe UserNotification vuota. Stub per sistema notifiche utente. Potrebbe essere obsoleto.

# GUI/WebUpdatePlugin
File vuoto (1 riga vuota). Probabilmente obsoleto o placeholder non implementato. Candidato all'eliminazione.

# GUI/WebViewDialog
Panel wxWebView per browser integrato nel Plater: carica URL, navigazione back/forward/reload, ejecuta JavaScript custom, login callback con timer, supporto mDNS (Bonjour). Feature BBS per mostrare model mall/staff pick/design info. Dipende da wxWebView della piattaforma.

# GUI/WipeTowerDialog
Dialog configurazione wipe tower/ramming: `RammingPanel` con chart grafico volume/tempo customizzabile, `WipingPanel` matrice flushing colori con selezione automatica/manuale estrusore. Integra `RammingChart` per spline curve. Feature multi-materiale dedicata.

# GUI/wxExtensions
Header utility wxWidgets estese: `append_menu_item()` per menu dinamici con icon/callback, `create_scaled_bitmap()` per PNG/SVG con DPI scaling, `wxCheckListBoxComboPopup` combo con checkbox. Infrastruttura UI riutilizzabile. ~700 righe.

# GUI/Gizmos/GizmoObjectManipulation
Classe supporto manipolazione oggetto: cache Position/Rotation/Scale arrotondata, conversione unità imperiali/metriche, label stringhe per UI (move/rotate/scale). Non è gizmo OpenGL ma helper classe per sincronizzazione dati tra Selection e UI. Core 3D UX.

# GUI/Gizmos/GLGizmoAdvancedCut
Gizmo tagli avanzati: piano cut customizzabile con rotation, connettori cut con tipo/stile/shape configurable, depth/size/tolerance, preview mesh connector, segment smoothing. Eredita da `GLGizmoRotate3D`. Feature CAM avanzata BBS.

# GUI/Gizmos/GLGizmoAssembly
Gizmo assembly per montaggio multi-volume: eredita da `GLGizmoMeasure` (picking facce/spigoli), mode selector face-face/point-point, gestisce undo/redo enter/leave. Feature BBS per assemblaggio componenti 3D. Implementazione minimale stub.

# GUI/Gizmos/GLGizmoBase
Classe base di tutti i gizmo OpenGL: grabber system (enabled/dragging/color), raycasting per picking, state machine (Off/On), ImGui rendering window. ~400 righe header. Assoluto core per interazione 3D. Indispensabile.

# GUI/Gizmos/GLGizmoBrimEars
Gizmo brim ears (punti custom brim per evitare warping): `CacheEntry` punti con selezione/normal/hover state, aggiunta/eliminazione/modifica punti, unproject su mesh. Feature support print quality. Editing brim intelligente.

# GUI/Gizmos/GLGizmoCut
Gizmo cut 3D con piano customizzabile: grabber per posizione/rotation piano, connettori boolean (union/difference/intersection) con depth ratio/tolerance, segment smoothing alpha, preview mesh result. Feature CAM/boolean operations. ~800 righe header.

# GUI/Gizmos/GLGizmoEmboss
Gizmo emboss testo 3D: estrae font da disco, stili emboss configurabili (profondità/altezza), surface drag su mesh, italic/bold toggle, anteprima rendering testo. Integra EmbossStyleManager, TextLines per visualizzazione. Feature design 3D avanzata.

# GUI/Gizmos/GLGizmoFaceDetector
Gizmo face detection minimalista: rileva facce top/bottom da normale mesh, sample interval regolabile. Stub incomplete, rendering GUI minimale. Feature BBS per supporti automatici riconoscimento orientamento.

# GUI/Gizmos/GLGizmoFdmSupports
Gizmo paint-on supports: eredita da `GLGizmoPainterBase`, painting angle-based support auto-generation, threshold angle customizzabile, preview volume con regenerate in thread background. Feature supporti painting interattivi con auto-generation intelligente.

# GUI/Gizmos/GLGizmoFlatten
Gizmo flatten per rilevare piani su mesh: calcola piani principali mesh, preview con picking, rotation interattivo su piano selezionato. Feature auto-orienting oggetti su piatti inclinati. Estrae versione mesh/transform per caching ricalcolo.

# GUI/Gizmos/GLGizmoFuzzySkin
Gizmo paint-on fuzzy skin: eredita da `GLGizmoPainterBase`, painting rugosità superficiale su mesh, applicazione undo/redo. Feature superfici ruvide per texture stampate. Implementazione painting base simile a supports.

# GUI/Gizmos/GLGizmoHollow
Gizmo SLA hollowing: gestisce drain holes (buchi drenaggio), aggiunta/eliminazione/selezione punti, clipping plane, preview mesh cavo. Feature SLA rimozione supporti e drenaggio interno. Usa libslic3r::sla::DrainHoles.

# GUI/Gizmos/GLGizmoMeasure
Gizmo misure avanzate: picking feature geometriche (punti/spigoli/cerchi/piani), calcolo distanze/angoli, rendering annotazioni con colori (selezione 1st/2nd/neutro/hover), assembly mode face-face/point-point. Feature dimensionamento stampati.

# GUI/Gizmos/GLGizmoMeshBoolean
Gizmo boolean operations: seleziona volume source/tool, operazione union/difference/intersection, preview mesh result, genera nuovo volume con delete opzionale input. Feature CAD boolean modeling. State machine select-mode.

# GUI/Gizmos/GLGizmoMmuSegmentation
Gizmo paint-on color segmentation: eredita `GLGizmoPainterBase`, painting estrusori multi-color (up to 16), remap filamenti feature, detect edge geometry toggle, extruder color palette. Core MMU painting interattivo BBS.

# GUI/Gizmos/GLGizmoMove
Gizmo move 3D oggetti: translation su assi XYZ con grabber connection visualization, snap step customizzabile, bounding box tracking, integra `GizmoObjectManipulation` per UI sync. Indispensabile per editing posizione.

# GUI/Gizmos/GLGizmoPainterBase
Classe base gizmo painting: `TriangleSelectorGUI` per rendering triangle picking/painting su mesh, painting enforcers/blockers/seed-fill con GLModel, wire frame toggle. Base per seam/supports/fuzzy/MMU painting. Core painting framework.

# GUI/Gizmos/GLGizmoScale
Gizmo scale 3D multasse: scale uniforme/singolo asse con pivot selection, scaling in place vs bottom plane, grabber connection lines, snap step. Integra `GizmoObjectManipulation` per UI sync. Indispensabile per ridimensionamento.

# GUI/Gizmos/GLGizmoSeam
Gizmo paint-on seam placement: eredita `GLGizmoPainterBase`, painting linea cucitura preferenziale su mesh, undo/redo. Feature controllo posizione cucitura per stampe. Minimalista ma funzionale.

# GUI/Gizmos/GLGizmoSimplify
Gizmo semplificazione mesh: riduzione triangoli con ratio % o count assoluto, errore quadrico customizzabile, preview wireframe, worker thread background, notifica suggestion. Feature ottimizzazione mesh. ~500 righe header.

# GUI/Gizmos/GLGizmoSlaSupports
Gizmo SLA support points (editing): `CacheEntry` punti supporto con head diameter, aggiunta/eliminazione/selezione, unproject su mesh, density customizzabile. Feature SLA supporti manuali. Simile a BrimEars per punti picking.

# GUI/Gizmos/GLGizmoSVG
Gizmo SVG emboss import e placement: carica file SVG, conversione mesh, surface drag su oggetto, depth/scaling, create_volume su mouse position. Feature design vettoriale su 3D. Simile a Emboss ma per SVG.

# GUI/Gizmos/GLGizmoText
Gizmo text rendering 3D: input testo, font selection combo, size/thickness/depth/italic/bold parametri, surface text toggle (flat vs surface), position/rotation dragging. Feature text 3D design. Complemento Emboss.

# GUI/Gizmos/GLGizmos
Header evento enum `SLAGizmoEventType` (LeftDown/LeftUp/RightDown/Dragging/Delete/SelectAll/ApplyChanges/ecc.), include file gizmo principali. Core enum evento interaction framework. Stub header minimale.

# GUI/Gizmos/GLGizmosCommon
Pool data comuni gizmo: enum `CommonGizmosDataID` (SelectionInfo/InstancesHider/Raycaster/ObjectClipper), `CommonGizmosDataPool` gestione creazione/update/release data. Core data sharing tra gizmo. Infrastruttura essential.

# GUI/Gizmos/GLGizmosManager
Manager centrale gizmo: enum `EType` (Move/Rotate/Scale/Flatten/Cut/Seam/FuzzySkin/MMU/Emboss/ecc.), toolbar GL integrato, CommonGizmosDataPool, enable/disable gizmo by type. ~900 righe header. Orchestrator gizmo system.

# GUI/Jobs/ArrangeJob
Job background arrange oggetti: prepara poligoni selected/unselected/unprintable/locked, computation arranging multi-plate support, finalize con update istanze. Feature posizionamento automatico. Core arrange functionality.

# GUI/Jobs/BoostThreadWorker
Worker thread Boost.Thread: implementa `Worker` interface, esecue Job in thread background, `JobQueue` input, `MessageQueue` output comunicazione UI/worker. Core background job execution threading. ~200 righe.

# GUI/Jobs/BusyCursorJob
Template wrapper Job `BusyCursored`: RAII `CursorSetterRAII` mostra wxBusyCursor durante esecuzione job, ripristina al completamento. Feature UX feedback durante operazioni lunghe. Semplice ma utile.

# GUI/Jobs/CreateFontNameImageJob
Job rasterizzazione testo font: `FontImageData` input (text/font/encoding/texture_id/size), rasterizza in image buffer, copia texture OpenGL. Feature font texture generation per emboss UI. Supporto CJK.

# GUI/Jobs/CreateFontStyleImagesJob
Job creazione texture style emboss: `StyleManager::StyleImagesData` input, rasterizza stili emboss (testo con styling), produce texture atlas con sub-texture descriptors. Feature emboss style preview UI.

# GUI/Jobs/EmbossJob
Job creazione volume embossato: `DataBase` abstract, `DataCreateVolume` concreta con shape EmbossShape, creation in Plater, surface projection direction (raised/engraved), from_surface offset. Feature text/SVG 3D creation.

# GUI/Jobs/FillBedJob
Job riempire piatto: seleziona oggetto, clona e arranga copie fino pieno piatto, polygon arrange logic, multi-plate support. Feature riempimento letto automatico. Simple wrapper ArrangeJob per single object.

# GUI/Jobs/Job
Classe base astratta Job: interfaccia `Ctl` controller (update_status/was_canceled/call_on_main_thread), metodi `process()` (worker thread) e `finalize()` (UI thread). Core async job framework. ~60 righe header.

# GUI/Jobs/NotificationProgressIndicator
Implementazione `ProgressIndicator` basata `NotificationManager`: progress bar come notifica ImGui, error handling UI, cancel callback. Feature progress feedback non-intrusive. Integrazione notifications system.

# GUI/Jobs/OrientJob
Job background orientamento oggetti: seleziona oggetti selected/unselected/unprintable, OrientMesh per mesh + setter trasformazioni, preparazione su piatto singolo. Feature orientamento automatico per qualità stampa. Wrapper orientation library.

# GUI/Jobs/PlaterWorker
Worker template wrapper `PlaterWorker<WorkerSubclass>`: `PlaterJob` wrapper timing process/finalize, wxWakeUpIdle events, BusyCursored integrato. Logging durata job millisecond precision. Feature Plater UI job integration.

# GUI/Jobs/ProgressIndicator
Classe base astratta `ProgressIndicator`: interfaccia progress tracking (set_range/set_progress/set_status_text/set_cancel_callback/clear_percent/show_error_info). Core progress indication framework. ~30 righe.

# GUI/Jobs/RotoptimizeJob
Job ottimizzazione rotazione SLA: enum metodi (best surface quality/least supports/lowest Z height), FindFn callback per orientation::find_*, accuracy customizzabile. Feature SLA automatic model rotation optimization. Integra libslic3r SLA.

# GUI/Jobs/SLAImportDialog
Dialog import SLA archive (SL1/SL1S/ZIP): file picker, combo selection import mode (model+profile/profile only/model only), quality dropdown (accurate/balanced/quick), Get marchsq window size per mesh-to-volume conversion.

# GUI/Jobs/SLAImportJob
Job import SLA: `SLAImportJobView` interfaccia (get_selection/get_path/get_marchsq_windowsize), `prepare()` carica archive, `process()` mesh conversion, `finalize()` integra volumi Plater. Feature SLA project import.

# GUI/Jobs/ThreadSafeQueue
Template thread-safe queue SPSC (single producer single consumer): `consume_one(fn, BlockingWait)` con timeout/pop_flag atomica, `push(element)` notify cv. Base threading message queue BBS. Semplice ma efficace.

# GUI/Jobs/UpgradeNetworkJob
Job upgrade download/install plugin network: enum `PluginInstallStatus` (normal/failed/completed/unzipped), InstallProgressFn callback, event dichiarazione EVT_UPGRADE_*. Feature BBS plugin auto-update background download.

# GUI/Jobs/Worker
Interfaccia astratta `Worker`: `push()` queue job, `is_idle()` status, `cancel()/cancel_all()`, `process_events()` UI thread, `wait_for_current/wait_for_idle()` blocking wait. Helper `queue_job()` template lambda wrapper. Core worker framework.

# GUI/Widgets/AxisCtrlButton
Button custom axis control: outer/inner ring concentric, 8 positions (UP/DOWN/LEFT/RIGHT + quadranti interni), HOME button center, StateColor/StateHandler rendering custom. Feature 3D navigation pad-like control.

# GUI/Widgets/Button
Button custom: ButtonStyle (Regular/Confirm/Alert/Disabled), ButtonType (Compact/Window/Choice/Parameter/Expanded), icon attivi/inattivi, text color, selected state, può focus. Feature UI consistency custom styling.

# GUI/Widgets/CheckBox
CheckBox custom wxBitmapToggleButton: `SetHalfChecked()` three-state (on/half/off), bitmap state-dependent DPI-scaled, macOS custom handling, rescale support. Feature tri-state checkbox UI widget.

# GUI/Widgets/ComboBox
ComboBox custom: TextInput + DropDown integrati, items con bitmap/client data/tooltip, SetValue/GetValue/SetSelection, drop_down flag, text off mode. Feature rich combo widget multi-column.

# GUI/Widgets/DialogButtons
Panel bottoni dialog standard: DialogButtons(parent, labels, primary_btn), GetButtonFromID/GetButtonFromLabel, GetOK()/GetYES()/GetCANCEL()/ecc, SetPrimaryButton/SetAlertButton, map standard ID. Feature easy dialog button creation.

# GUI/Widgets/DropDown
Dropdown custom PopupWindow: items lista (texts/tips/icons/bitmap check), selection/hover, CornerRadius, border color, align icon, text off mode, use_content_width toggle, DPI rescale. Feature customizable dropdown widget.

# GUI/Widgets/ErrorMsgStaticText
Panel semplice visualizzazione errori: wxPanel + paint event, SetLabel per messaggio errore, rendering custom. Minimale ma specifico error message display widget.

# GUI/Widgets/ImageSwitchButton
Button switch on/off con immagini: `SetLabels()` per label on/off, `SetImages()` immagini custom, `SetValue()` stato, text color customizzabile. Anche `FanSwitchButton` variante con speed control (fan valori).

# GUI/Widgets/Label
Label custom wxStaticText: font customizzabile, style (hyperlink/propagate mouse/auto wrap), wrap(width), split_lines utility statica, sistema font statici (Head_**/Body_**) con scale. Feature typography consistency.

# GUI/Widgets/LabeledStaticBox
StaticBox custom: wxStaticBox + corner radius + border width/color + state handlers + font customizzabile, DrawBorderAndLabel override, GetBordersForSizer. Feature stylized container widget.

# GUI/Widgets/PopupWindow
PopupWindow thin wrapper wxPopupTransientWindow: Create(parent, flags), GTK top window activate handling. Minimale base per popup windows. ~30 righe header.

# GUI/Widgets/ProgressBar
ProgressBar rendering custom: progress bar con colore customizzabile, raggio arrotondato, visualizza percentuale opzionale, disabilitato mode con testo, SetValue/Reset/SetRadius. Feature progress visualization widget.

# GUI/Widgets/ProgressDialog
ProgressDialog custom: wxDialog + gauge + cancel button, modalità 1line/2line adattiva, scrolled message panel, progress tracking Update/Pulse. Feature progress dialog UI wrapper.

# GUI/Widgets/RadioBox
RadioBox custom wxBitmapToggleButton: on/off bitmap state-dependent, Rescale DPI, Enable/Disable. Minimale radio button widget. Simile CheckBox ma singola selezione.

# GUI/Widgets/RadioGroup
RadioGroup panel: lista radio button (orizzontale/verticale) con label button, selezione singola, SelectNext/SelectPrevious, SetRadioTooltip, state disabled/enabled. Feature radio selection group.

# GUI/Widgets/RoundedRectangle
RoundedRectangle minimale: wxWindow con corner radius customizzabile, colore sfondo, tipo rendering. Thin wrapper per visualizzare rettangolo arrotondato. ~30 righe.

# GUI/Widgets/Scrollbar
MyScrollbar custom scrollbar: track/tip colore, virtual dimension tracking, mouse drag/wheel, margin/scrollbar width, posizione thumb. Feature custom scrollbar render.

# GUI/Widgets/ScrolledWindow
ScrolledWindow wxScrolled custom: integra MyScrollbar right/bottom, SetVirtualSize, margin width, scrollbar colore/tip. Panel user content. Feature scrolled container widget.

# GUI/Widgets/SideButton
SideButton sidebar button: text/icon layout, corner radius partial (selettive), text horizontal orientation, border/background color state-driven, bottom color. Feature sidebar navigation button.

# GUI/Widgets/SideMenuPopup
SideMenuPopup PopupWindow: lista SideButton, append_button, OnDismiss/Show/ProcessLeftDown. Feature sidebar context menu popup.

# GUI/Widgets/SpinInput
SpinInput spin box: TextCtrl + Button up/down, SetValue/GetValue, SetRange/SetStep, mouse wheel/key support, timer hold-down acceleration. Feature number input spinner widget.

# GUI/Widgets/StateColor
StateColor stato→colore mapper: enum State (Normal/Enabled/Checked/Focused/Hovered/Pressed/Disabled/ecc), colorForStates lookup, LAB color difference utility statica. Core state-driven color system.

# GUI/Widgets/StateHandler
StateHandler gestore stato: wxEvtHandler, attach StateColor/children, update_binds, states() tracking, EVT_ENABLE_CHANGED. Feature centralized state change notification system.

# GUI/Widgets/StaticBox
StaticBox wxWindow base container: corner radius, border width/color, background color (2 layers), state-driven appearance, eraseEvent/paintEvent custom rendering. Base classe widget stile.

# GUI/Widgets/StaticLine
StaticLine divisore: orizzontale/verticale, optional label/icon, line colore, Rescale, render custom. Feature divider separator widget.

# GUI/Widgets/StepCtrl
StepCtrl step slider: `StepCtrlBase` abstract (steps/tips/bar), `StepCtrl` con thumb draggabile, `StepIndicator` con OK checkmark, AppendItem/SelectItem/GetSelection. Feature step control wizard widget.

# GUI/Widgets/SwitchButton
SwitchButton toggle on/off: wxBitmapToggleButton, label on/off customizzabile, track/thumb color state-driven, bitmap on/off DPI-scaled. Feature switch/toggle button widget.

# GUI/Widgets/TabCtrl
TabCtrl tab buttons: lista Button tabs, image list support, AppendItem/DeleteItem/SelectItem, bold font variant, GetItemData/SetItemData. Feature tab control widget.

# GUI/Widgets/TempInput
TempInput temperatura input: wxTextCtrl + label/icon attivo/inattivo, min/max temperature range, warning mode (too high/low), readonly toggle, SetCurrTemp/SetTagTemp. Feature temperature input widget.

# GUI/Widgets/TextInput
TextInput text field: wxTextCtrl + label/icon customizzabile, label/text color state-driven, corner radius, enable/disable, Rescale. Feature text input widget base.

# GUI/Widgets/WebView
WebView helper thin wrapper: CreateWebView(parent, url) factory, CheckWebViewRuntime/DownloadAndInstallWebViewRuntime Windows-specific, LoadUrl/RunScript, RecreateAll. Feature wxWebView helper utilities.
