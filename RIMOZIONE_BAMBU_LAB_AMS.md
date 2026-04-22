# Piano Finale di Pulizia Residui BambuLab e AMS

## Scopo del documento

Questo file deve guidare una IA nella pulizia finale dei residui BambuLab e AMS ancora presenti nel repository.

Il grosso della rimozione è già stato eseguito. Questo nuovo piano non deve ripetere gli step storici già completati, ma deve concentrarsi solo sulle parti ancora attive o sospette.

L'obiettivo non è rimuovere ogni stringa che contiene `Bambu`, `BBL`, `AMS` o `Ams`, ma eliminare solo:

- codice AMS ancora eseguito nel flusso preset/UI
- opzioni AMS ancora esposte all'utente
- branch vendor-specific Bambu non più giustificati
- endpoint, asset e risorse web residue chiaramente collegate al vecchio stack Bambu
- tipi e strutture dati BBL/AMS che risultano orfani o sovra-specifici

## Vincoli obbligatori

- **Non toccare l'About.**
- **Non rimuovere `BBLTopbar`.**
- **Non rimuovere `bbs_3mf` a meno che non venga deciso esplicitamente di abbandonare la compatibilità file relativa.**
- **Non cancellare automaticamente ogni simbolo `BBL*`.** Alcuni nomi sono solo storici.
- **Ogni step deve essere piccolo, autonomo e compilabile o quasi compilabile con pulizia locale limitata.**
- **Dopo ogni step eliminare include, forward declaration, enum, eventi, campi e funzioni diventati inutili.**
- **Eseguire uno step per volta.** Non accorpare step diversi in un unico intervento.

## Stato già verificato

Risultano già rimossi dal tree i principali workflow Bambu/cloud storici, inclusi:

- `WebUserLoginDialog`
- `BindDialog`
- `SelectMachine*`
- `SendToPrinter`
- `Monitor*`
- `StatusPanel`
- `DeviceManager`
- `UserManager`
- `NetworkAgent`
- `bambu_networking.hpp`

Questo documento quindi riparte solo dai residui ancora presenti.

## Regole decisionali

Per ogni step la IA deve distinguere tra tre casi:

- **Da rimuovere subito**
  - codice attivo chiaramente AMS/Bambu-specifico

- **Da verificare prima di rimuovere**
  - strutture condivise o compatibilità file/preset

- **Da lasciare**
  - branding storico innocuo
  - About
  - componenti generici ancora utili

## Checklist prima di modificare uno step

Prima di toccare uno step la IA deve verificare sempre:

- dove viene usato il simbolo o il file
- se il comportamento è ancora attivo a runtime oppure è solo testo/commento
- se il codice è AMS-specifico oppure implementa una funzione più generale da rinominare
- se esistono campi/config collegati in `PresetBundle`, `PrintConfig`, `AppConfig`, `Preferences`, `Plater`, `Tab`
- se lo step richiede aggiornamenti in `CMakeLists.txt`

## Fase 1 - Rimuovere il residuo AMS attivo nei preset

Questa è la fase più importante. Qui c'è ancora codice realmente eseguito.

### Step 1 - Pulire `PresetComboBoxes` dal supporto AMS

- **File target**:
  - `src/slic3r/GUI/PresetComboBoxes.hpp`
  - `src/slic3r/GUI/PresetComboBoxes.cpp`
- **Da rimuovere o riscrivere**:
  - `update_ams_color()`
  - `add_ams_filaments(...)`
  - `selected_ams_filament()`
  - campi `m_first_ams_filament`, `m_last_ams_filament`
  - label UI `AMS filaments`
  - gating con `is_bbl_vendor()` usato solo per AMS filaments
- **Da verificare**:
  - che il cambio preset filamento continui a funzionare senza rompere il colore filamento standard
- **Output atteso**:
  - nessuna UI preset deve mostrare filamenti AMS o logiche AMS implicite

### Step 2 - Pulire `PresetBundle` dalle strutture AMS residue

- **File target**:
  - `src/libslic3r/PresetBundle.hpp`
  - `src/libslic3r/PresetBundle.cpp`
- **Da rimuovere o ridisegnare**:
  - `filament_ams_list`
  - `ams_multi_color_filment`
  - `sync_ams_list(...)`
- **Attenzione**:
  - se una parte di `ams_multi_color_filment` serve in realtà a una funzione multi-colore generica, non mantenerla con semantica AMS: rinominarla e generalizzarla nello step successivo oppure rimuoverla completamente
- **Output atteso**:
  - `PresetBundle` non deve più contenere storage esplicitamente AMS-specifico

### Step 3 - Adeguare `Plater` dopo la rimozione AMS da `PresetBundle`

- **File target**:
  - `src/slic3r/GUI/Plater.cpp`
- **Da aggiornare**:
  - codice che legge `ams_multi_color_filment`
  - eventuale fallback per colori multi-materiale
- **Attenzione**:
  - evitare regressioni nel calcolo flush se il comportamento può essere ricondotto ai colori filamento standard
- **Output atteso**:
  - `Plater` non deve più dipendere da nomi o storage AMS

### Step 4 - Adeguare `WipeTowerDialog` dopo la rimozione AMS da `PresetBundle`

- **File target**:
  - `src/slic3r/GUI/WipeTowerDialog.cpp`
- **Da aggiornare**:
  - `calc_flushing_volumes()`
  - accessi a `ams_multi_color_filment`
- **Output atteso**:
  - nessun riferimento AMS in logica wipe/flush

## Fase 2 - Rimuovere opzioni AMS ancora esposte all'utente

### Step 5 - Rimuovere la preferenza `Skip AMS blacklist check`

- **File target**:
  - `src/slic3r/GUI/Preferences.cpp`
- **Da rimuovere**:
  - checkbox `Skip AMS blacklist check`
  - wiring UI collegato
- **Da cercare anche**:
  - letture/scritture della chiave `skip_ams_blacklist_check` nel resto del repository
- **Output atteso**:
  - nessuna preferenza AMS visibile all'utente

### Step 6 - Rimuovere eventuale config residua collegata ad AMS blacklist

- **File target da verificare**:
  - `src/libslic3r/AppConfig.hpp`
  - `src/libslic3r/AppConfig.cpp`
  - altri file trovati dalla ricerca della chiave `skip_ams_blacklist_check`
- **Regola**:
  - rimuovere solo se la chiave risulta ormai orfana
- **Output atteso**:
  - nessuna chiave config AMS rimasta senza consumatori legittimi

## Fase 3 - Rimuovere branch vendor-specific Bambu non più giustificati

### Step 7 - Pulire `Tab.cpp` dal branch `is_bbl_vendor()` rimasto

- **File target**:
  - `src/slic3r/GUI/Tab.cpp`
- **Da verificare e rimuovere se non più necessario**:
  - logica in `on_presets_changed()` che cambia render option e calibration overlay per vendor BBL
- **Attenzione**:
  - se una parte è utile in generale, estrarla da vendor detection e renderla neutra
- **Output atteso**:
  - nessun comportamento runtime dipendente da `is_bbl_vendor()` senza una motivazione ancora valida

### Step 8 - Valutare `PresetBundle::is_bbl_vendor()` e `VendorType::Marlin_BBL`

- **File target da verificare**:
  - `src/libslic3r/PresetBundle.hpp`
  - `src/libslic3r/PresetBundle.cpp`
  - eventuali enum/file vendor correlati trovati dai call site
- **Regola**:
  - non rimuovere se serve ancora alla compatibilità preset macchina
  - rimuovere solo se dopo lo step 7 non esistono più usi funzionali reali
- **Output atteso**:
  - eliminare solo il vendor-special casing runtime non più usato

## Fase 4 - Pulire strutture dati BBL/AMS ancora sospette

### Step 9 - Verificare `ProjectTask.hpp`

- **File target**:
  - `src/libslic3r/ProjectTask.hpp`
- **Elementi sospetti**:
  - `BBLProject`
  - `BBLProfile`
  - `BBLTask`
  - `BBLModelTask`
  - `BBLSubTask`
  - campi `ams_id` e `slot_id` in `FilamentInfo`
- **Metodo**:
  - prima mappare i call site reali
  - poi decidere se rimuovere, ridurre o lasciare per compatibilità
- **Regola**:
  - i campi AMS palesemente orfani vanno rimossi
  - i tipi BBL più grandi vanno rimossi solo se non più usati

### Step 10 - Rimuovere i campi AMS orfani da `FilamentInfo` se confermati inutili

- **File target**:
  - `src/libslic3r/ProjectTask.hpp`
  - eventuali `.cpp` che li usano
- **Da rimuovere**:
  - `ams_id`
  - `slot_id`
  - commenti che parlano di `new ams mapping`
- **Precondizione**:
  - nessun uso reale residuo

## Fase 5 - Pulire `GUI_App` dai residui cloud/web Bambu

### Step 11 - Verificare e rimuovere endpoint Bambu residui in `GUI_App.cpp`

- **File target**:
  - `src/slic3r/GUI/GUI_App.cpp`
- **Da verificare**:
  - URL `api.bambulab.*`
  - URL `makerhub-*`
  - `makerworld.com` se usato solo come parte del vecchio stack Bambu integrato
  - eventuali ping o test verso host Bambu
- **Regola**:
  - rimuovere solo gli endpoint non più raggiungibili da funzionalità ancora supportate
- **Output atteso**:
  - `GUI_App` non deve più contenere integrazioni cloud Bambu morte

### Step 12 - Pulire testo e codice morto residuo in `GUI_App.cpp`

- **File target**:
  - `src/slic3r/GUI/GUI_App.cpp`
- **Da pulire**:
  - commenti storici sulla rimozione Bambu
  - codice commentato non più utile
  - eventuali test di connettività o helper non più usati
- **Regola**:
  - non fare refactor largo; solo pulizia locale conseguente

## Fase 6 - Pulire risorse web e stringhe chiaramente legate al vecchio stack Bambu

### Step 13 - Verificare le risorse web login/plugin residue

- **File target candidati**:
  - `resources/web/data/text.js`
  - `resources/web/login/js/login.js`
- **Da verificare**:
  - stringhe `Bambu Network plug-in`
  - riferimenti a cloud Bambu
  - link a `www.bambulab.com`
- **Metodo**:
  - prima confermare se queste risorse sono ancora referenziate dal prodotto
  - se non lo sono, rimuoverle o ripulirle

### Step 14 - Pulire asset o stringhe web sicuramente orfani

- **File target**:
  - quelli confermati orfani nello step 13
- **Output atteso**:
  - nessuna risorsa UI/web deve ancora proporre installazione plugin Bambu o login Bambu se la feature non esiste più

## Fase 7 - Pulizia finale di coerenza

### Step 15 - Cercare chiavi orfane `ams_mapping`, `ams_mapping_info`, `task_use_ams`

- **File target da verificare**:
  - `src/libslic3r/PrintConfig.hpp`
  - `src/libslic3r/PrintConfig.cpp`
  - altri call site trovati da ricerca
- **Regola**:
  - rimuovere solo ciò che non serve più a compatibilità file o workflow reali

### Step 16 - Pulire `src/slic3r/CMakeLists.txt` e file di build solo se necessario

- **Da fare solo se**:
  - gli step precedenti hanno davvero eliminato sorgenti o dipendenze residue
- **Da verificare**:
  - riferimenti a file rimossi
  - dipendenze inutili sopravvissute alla pulizia

### Step 17 - Ricerca finale su tutto il repository

- **Ricerca finale obbligatoria** sui termini:
  - `AMS`
  - `Ams`
  - `filament_ams_list`
  - `ams_multi_color_filment`
  - `skip_ams_blacklist_check`
  - `is_bbl_vendor`
  - `Marlin_BBL`
  - `api.bambulab`
  - `Bambu Network`
- **Obiettivo**:
  - classificare ogni match residuo come:
    - legittimo
    - compatibilità da mantenere
    - cleanup ancora da fare

## Elementi che non vanno rimossi automaticamente

- `BBLTopbar`
- tutto l'`About`
- `bbs_3mf` se serve compatibilità file
- riferimenti a modelli stampante Bambu nei preset, se il supporto preset macchina locale deve restare
- testo tecnico descrittivo che cita Bambu solo come esempio, se non introduce feature residue

## Ordine consigliato di esecuzione

1. `PresetComboBoxes.*`
2. `PresetBundle.*`
3. `Plater.cpp`
4. `WipeTowerDialog.cpp`
5. `Preferences.cpp`
6. config collegata a `skip_ams_blacklist_check`
7. `Tab.cpp`
8. valutazione `is_bbl_vendor()` / `Marlin_BBL`
9. verifica `ProjectTask.hpp`
10. rimozione campi AMS orfani in `FilamentInfo`
11. endpoint Bambu residui in `GUI_App.cpp`
12. codice morto/commenti residui in `GUI_App.cpp`
13. verifica risorse web
14. rimozione risorse web orfane
15. ricerca chiavi config AMS residue
16. pulizia CMake se necessaria
17. ricerca finale repository-wide

## Regola di esecuzione per l'IA

Per ogni richiesta operativa, la IA deve eseguire **un solo step per volta** seguendo questo schema:

1. rileggere i file target dello step
2. cercare i call site dei simboli coinvolti
3. applicare una modifica minima e coerente
4. pulire include e riferimenti orfani locali
5. riportare cosa è stato rimosso, cosa è stato mantenuto e perché

## Esito atteso finale

Alla fine devono sparire:

- il residuo AMS nei preset e nella logica multi-colore AMS-specifica
- le preferenze AMS residue
- i branch runtime `is_bbl_vendor()` non più giustificati
- gli endpoint e le risorse web Bambu non più usati
- i campi e le strutture AMS/BBL rimasti orfani

Alla fine devono restare:

- slicing locale
- gestione preset e stampanti ancora supportate localmente
- compatibilità file eventualmente necessaria
- UI generale condivisa
- `BBLTopbar`
- tutto l'`About`
