# AABBMesh.cpp
Wrapper attorno a un indexed_triangle_set che costruisce un albero AABB (Axis-Aligned Bounding Box) per query spaziali efficienti su mesh 3D. Espone operazioni di ray-casting (primo impatto e tutti gli impatti), calcolo della distanza quadratica al punto più vicino sulla mesh, e accesso a vertici/indici/normali per faccia. Internamente usa AABBTreeIndirect e igl::Hit. Supporta opzionalmente il ray-casting attraverso fori SLA (SLIC3R_HOLE_RAYCASTER), filtrando gli impatti con DrainHole tramite logica di annidamento entry/exit. Usato ampiamente dal sistema di supporti SLA e da altri moduli che necessitano di intersezioni geometriche rapide.

# Algorithm/LineSplit.cpp
Implementa la funzione do_split_line che taglia una polilinea (open o closed) usando un insieme di ExPolygon come regione di clip, restituendo una SplittedLine: sequenza di punti annotati con flag clipped e indice sorgente. Usa ClipperLib_Z (Clipper con coordinate Z) per tracciare la provenienza di ogni punto (sorgente, clip o intersezione nuova). Risolve i punti d'intersezione con un AABBTree sulle linee originali, riconnette i segmenti clippati in ordine corretto rispetto al percorso originale. Usato per segmentare le linee di infill/perimetro in parti interne/esterne a regioni di confine.

# Algorithm/RegionExpansion.cpp
Implementa la propagazione a onde (wave expansion) di regioni sorgente all'interno di regioni di confine. La funzione wave_seeds trova i semi d'intersezione tra sorgenti espanse e confini usando ClipperLib_Z. propagate_waves esegue l'espansione iterativa via ClipperOffset (round), clippando il fronte d'onda al confine. Fornisce anche propagate_waves_ex (risultati come ExPolygon), expand_expolygons e merge_expansions_into_expolygons. Usato per espandere bridge/solid infill nei confini adiacenti.

# AppConfig.cpp
Gestisce la configurazione persistente dell'applicazione (file JSON con sezioni app, presets, filaments, models, orca_presets). Implementa load() (parsing JSON con fallback su backup .bak e checksum MD5 su Win32), save() (scrittura atomica via file PID + rename), set_defaults() (valori di default per tutte le opzioni UI/comportamento). Gestisce anche le preferenze utente come lingua, camera, shortcuts mouse/tastiera, backup automatico, modalità developer e aggiornamenti. Mantiene m_storage (mappa sezione→chiave→valore), m_vendors, m_filament_presets, m_printer_settings.

# Arachne/BeadingStrategy/BeadingStrategy.cpp
Classe base astratta per le strategie di beading del motore Arachne (variable-width walls). Definisce l'interfaccia comune: compute(thickness, bead_count) restituisce Beading (larghezze e posizioni toolpath), getOptimalThickness, getTransitionThickness, getOptimalBeadCount, getTransitioningLength, getTransitionAnchorPos, getNonlinearThicknesses. I parametri chiave sono optimal_width, wall_split/add_middle_threshold, default_transition_length e transitioning_angle. Fornisce implementazioni di default per i metodi non virtuali puri.

# Arachne/BeadingStrategy/BeadingStrategyFactory.cpp
Factory che costruisce la catena di BeadingStrategy per Arachne. Applica in ordine: DistributedBeadingStrategy (distribuzione del materiale), RedistributeBeadingStrategy (ottimizza larghezza pareti esterne), opzionalmente WideningBeadingStrategy (pareti sottili), opzionalmente OuterWallInsetBeadingStrategy (offset parete esterna), e infine LimitedBeadingStrategy (limite massimo numero di bead). Il pattern decorator consente di comporre le strategie in modo modulare in base ai parametri di stampa.

# Arachne/BeadingStrategy/DistributedBeadingStrategy.cpp
Strategia di beading che distribuisce il materiale in eccesso/difetto tra tutte le linee usando un peso gaussiano centrato sulla linea centrale. Per bead_count>2 calcola pesi proporzionali a (1 - dist_from_middle²/r²), distribuendo lo spessore residuo proporzionalmente. Per 1 o 2 bead usa divisione semplice. getOptimalBeadCount aggiunge una linea extra se lo spazio residuo supera la soglia di split/add configurata.

# Arachne/BeadingStrategy/LimitedBeadingStrategy.cpp
Meta-strategia decorator che limita il numero massimo di bead a max_bead_count. Se bead_count supera il limite, calcola il beading all'optimum per max_bead_count e inserisce due pareti di larghezza zero come marker (una per lato) per indicare il confine dell'area a larghezza variabile. Garantisce simmetria per bead count pari e dispari. Applicata sempre come ultimo wrapper nella catena della factory.

# Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.cpp
Meta-strategia decorator che applica un offset (inset o outset) alla posizione della parete esterna più esterna. Aggiusta toolpath_locations[0] di outer_wall_offset, limitandola a non superare la linea centrale. Non ha effetto con un solo bead. Usata per compensare eventuali scostamenti dimensionali della parete esterna rispetto al profilo teorico.

# Arachne/BeadingStrategy/RedistributeBeadingStrategy.cpp
Meta-strategia decorator che riserva le due pareti esterne con larghezza optimal_width_outer fissa, delegando le pareti interne alla strategia parent. Gestisce i casi 0, 1, 2 bead separatamente. Corregge getOptimalThickness, getTransitionThickness e getOptimalBeadCount per tenere conto delle due pareti esterne. Garantisce che il left_over finale sia coerente con la somma delle larghezze prodotte.

# Arachne/BeadingStrategy/WideningBeadingStrategy.cpp
Meta-strategia decorator per la stampa di pareti sottili. Se lo spessore è inferiore a optimal_width ma superiore a min_input_width, produce un singolo bead di larghezza max(thickness, min_output_width) invece di zero bead. Modifica getTransitionThickness(0) a min_input_width e getOptimalBeadCount a restituire 1 per spessori nel range [min_input, optimal]. getNonlinearThicknesses aggiunge min_output_width alla lista del parent.

# Arachne/SkeletalTrapezoidation.cpp
Cuore del motore Arachne: converte poligoni di input in un grafo di trapezoidazione scheletrica (basato sul diagramma di Voronoi) e genera i toolpath a larghezza variabile per le pareti. Costruisce il grafo half-edge dal Voronoi, calcola distanze al contorno, identifica gli archi centrali, propaga il bead count lungo lo scheletro, genera transizioni di larghezza e produrre le ExtrusionLine finali. File molto grande (~2100 righe), punto critico della pipeline Arachne.

# Arachne/SkeletalTrapezoidationGraph.cpp
Implementa il grafo half-edge (STHalfEdgeGraph) usato dalla trapezoidazione scheletrica. Fornisce operazioni di navigazione sul grafo: canGoUp/isUpward (direzione rispetto alla distanza al contorno), distToGoUp (distanza verso nodi più lontani), markCentral (marcatura archi centrali), per costruire il grafo da un diagramma di Voronoi e generare la struttura topologica necessaria ad Arachne.

# Arachne/utils/ExtrusionLine.cpp
Rappresenta una linea di estrusione a larghezza variabile come sequenza di ExtrusionJunction (posizione + larghezza). Implementa getLength(), simplify() (Douglas-Peucker con vincolo su area di estrusione deviata), calculateExtrusionAreaDeviationError() (errore di area per rimozione di un vertex), is_contour() (orientamento CW=contorno), area(). Fornisce anche extrusion_paths_append() per convertire ExtrusionLine/ClipperPaths in ExtrusionPaths Slic3r tramite ThickPolyline.

# Arachne/utils/PolylineStitcher.cpp
Specializzazioni template di PolylineStitcher per due tipi: VariableWidthLines (ExtrusionLine) e Polygons. Definisce le policy canReverse (le linee odd possono essere invertite), canConnect (le linee possono essere collegate solo se entrambe odd o entrambe even), isOdd. Usato per connettere segmenti di toolpath aperti in polilinee più lunghe o anelli chiusi dopo il processo di beading Arachne.

# Arachne/utils/SquareGrid.cpp
Griglia spaziale a celle quadrate per query di prossimità 2D. Converte coordinate in celle (toGridPoint/toGridCoord), itera su tutte le celle attraversate da un segmento (processLineCells, algoritmo di rasterizzazione a linea "fat"), e su tutte le celle in un raggio (processNearby). Usata come struttura dati di accelerazione per le ricerche di prossimità nel processo di beading Arachne.

# Arachne/WallToolPaths.cpp
Orchestratore del processo Arachne per la generazione di toolpath a larghezza variabile. make_paths_params costruisce i parametri dal PrintObjectConfig. WallToolPaths::generate() esegue: pre-processing dei poligoni, SkeletalTrapezoidation, post-processing (simplify, stitch, filter), separazione in inset/closing paths. Gestisce anche il patching di gap tra pareti. Punto di ingresso principale per la pipeline Arachne nel contesto dello slicer.

# ArcFitter.cpp
Algoritmo di approssimazione di polilinee con archi di cerchio (G2/G3). do_arc_fitting itera sui punti tentando di estendere l'arco corrente (ArcSegment::try_create_arc); quando l'arco non è più valido, salva il segmento precedente e ricomincia. Gestisce i casi degeneri (< 3 punti → linea retta). I risultati sono PathFittingData (start/end index + tipo Linear/Arc). Usato per generare G-code con comandi arco al posto di sequenze di segmenti lineari.
# Arrange.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# BlacklistedLibraryCheck.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# BoundingBox.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# BridgeDetector.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Brim.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# BuildVolume.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# calib.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Circle.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Clipper2Utils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# clipper.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ClipperUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Color.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Config.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# CustomGCode.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# CutSurface.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# CutUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# EdgeGrid.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ElephantFootCompensation.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Emboss.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ExPolygonCollection.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ExPolygon.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ExPolygonsIndex.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Extruder.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ExtrusionEntityCollection.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ExtrusionEntity.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ExtrusionSimulator.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# FaceDetector.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Feature/FuzzySkin/FuzzySkin.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Feature/Interlocking/InterlockingGenerator.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Feature/Interlocking/VoxelUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/Fill3DHoneycomb.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillAdaptive.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillBase.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillConcentric.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillConcentricInternal.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/Fill.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillCrossHatch.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillGyroid.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillHoneycomb.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillLightning.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillLine.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillPlanePath.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillRectilinear.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillTpmsD.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/FillTpmsFK.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/Lightning/DistanceField.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/Lightning/Generator.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/Lightning/Layer.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Fill/Lightning/TreeNode.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Flow.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# FlushVolCalc.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/3mf.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/AMF.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/bbs_3mf.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/OBJ.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/objparser.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/SL1.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/STEP.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/STL.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/svg.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Format/ZipperArchiveImport.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/AdaptivePAInterpolator.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/AdaptivePAProcessor.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/AvoidCrossingPerimeters.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/ConflictChecker.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/CoolingBuffer.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/FanMover.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/GCodeProcessor.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/PchipInterpolatorHelper.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/PostProcessor.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/PressureEqualizer.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/PrintExtents.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCodeReader.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/RetractWhenCrossingPerimeters.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/SeamPlacer.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCodeSender.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/SmallAreaInfillFlowCompensator.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/SpiralVase.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/ThumbnailData.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/Thumbnails.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/ToolOrdering.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/WipeTower2.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCode/WipeTower.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# GCodeWriter.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Geometry/Circle.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Geometry/ConvexHull.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Geometry.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Geometry/MedialAxis.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Geometry/Voronoi.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Geometry/VoronoiOffset.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Geometry/VoronoiUtilsCgal.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Geometry/VoronoiUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# IntersectionPoints.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# JumpPointSearch.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Layer.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# LayerRegion.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# libslic3r.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Line.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# LocalesUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Measure.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# MeshBoolean.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# MinAreaBoundingBox.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# MinimumSpanningTree.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# miniz_extension.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ModelArrange.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Model.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# MultiMaterialSegmentation.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# MultiPoint.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# MutablePolygon.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# NormalUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# NSVGUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ObjectID.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# OpenVDBUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Orient.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ParameterUtils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# pchheader.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PerimeterGenerator.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PlaceholderParser.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Platform.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PNGReadWrite.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Point.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Polygon.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PolygonTrimmer.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Polyline.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PresetBundle.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Preset.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PrincipalComponents2D.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PrintApply.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PrintBase.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PrintConfig.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Print.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PrintObject.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PrintObjectSlice.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# PrintRegion.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ProjectTask.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# QuadricEdgeCollapse.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Semver.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Shape/TextShape.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ShortEdgeCollapse.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# ShortestPath.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/Clustering.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/ConcaveHull.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/Hollowing.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/IndexedMesh.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/Pad.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLAPrint.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLAPrintSteps.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/RasterBase.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/RasterToPolygons.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/Rotfinder.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/SpatIndex.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/SupportPointGenerator.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/SupportTreeBuilder.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/SupportTreeBuildsteps.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/SupportTree.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SLA/SupportTreeMesher.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SlicesToTriangleMesh.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SlicingAdaptive.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Slicing.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Support/SupportCommon.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Support/SupportMaterial.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Support/SupportSpotsGenerator.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Support/TreeModelVolumes.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Support/TreeSupport3D.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Support/TreeSupport.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SurfaceCollection.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Surface.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# SVG.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Tesselate.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Thread.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Time.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Timer.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# TriangleMesh.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# TriangleMeshSlicer.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# TriangleSelector.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# TriangleSetSampling.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# TriangulateWall.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Triangulation.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# TryCatchSignal.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# TryCatchSignalSEH.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# utils.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# VariableWidth.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# Zipper.cpp
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo
# 
TODO: analizza il file e in massimo 1000 caratteri descrivine il contenuto/scopo