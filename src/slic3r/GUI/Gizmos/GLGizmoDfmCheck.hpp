#ifndef slic3r_GLGizmoDfmCheck_hpp_
#define slic3r_GLGizmoDfmCheck_hpp_

// "Print Check" gizmo: educational Design-for-Manufacturing feasibility analysis for
// large-scale pellet printing. Runs the DfmAnalyzer on the selected instance on a worker
// thread and paints the issues on the model: flat red/orange for walls thinner than
// 1x/2x the nozzle diameter, and per-degree color gradients over the full 0..90° lean
// range (no thresholds) for external (pale->dark red) and internal (pale->dark blue)
// overhangs — a flat top surface is the worst internal overhang (90°).

// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code,
// which overrides our localization "L" macro.
#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/DfmAnalyzer.hpp"
#include "libslic3r/Point.hpp"

#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace Slic3r {
class ModelObject;

namespace GUI {

class GLGizmoDfmCheck : public GLGizmoBase
{
public:
    GLGizmoDfmCheck(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);
    virtual ~GLGizmoDfmCheck();

protected:
    virtual std::string on_get_name() const override;
    virtual bool on_init() override { return true; }
    virtual void on_render() override;
    virtual void on_render_input_window(float x, float y, float bottom_limit) override;
    virtual bool on_is_activable() const override;
    virtual void on_set_state() override;
    virtual void data_changed(bool is_serializing) override;
    CommonGizmosDataID on_get_requirements() const override;

private:
    // Identity of a measurement: object + instance + volume meshes + the linear (rotation
    // and scale, no translation) part of each volume's world transform. Thickness and
    // overhang are translation-invariant, so moving the object never invalidates results.
    struct AnalysisKey
    {
        ObjectID object_id;
        int      instance_idx = -1;
        // volume id + facet count (facet count changes when the mesh is edited in place)
        std::vector<std::pair<ObjectID, size_t>> volumes;
        std::vector<Matrix3d>                    linear_parts;

        bool valid() const { return instance_idx >= 0; }
        bool operator==(const AnalysisKey &rhs) const;
        bool operator!=(const AnalysisKey &rhs) const { return !(*this == rhs); }
    };

    // Accessed by both the UI and the worker thread, guarded by m_state_mutex
    // (GLGizmoSimplify threading pattern).
    struct State
    {
        enum Status { idle, running, cancelling };
        Status                          status = idle;
        int                             progress = 0;
        AnalysisKey                     key; // key the worker is computing for
        std::unique_ptr<DfmMeasurement> result;
    };

    // Per-degree flat-colored overlay models built from the volume's local mesh and
    // rendered with the volume's current world matrix.
    struct VolumeOverlay
    {
        ObjectID volume_id;
        GLModel  thin_critical;
        GLModel  thin_warning;
        std::map<int, GLModel> external_by_deg; // lean degree -> bucket model
        std::map<int, GLModel> internal_by_deg;
    };

    static std::optional<AnalysisKey> make_key(const Selection &selection);
    const ModelObject *find_analyzed_object() const;

    void process();
    void stop_worker_thread_request();
    void worker_finished();
    void reclassify();
    void rebuild_overlays();
    void request_rerender(bool force = false);
    void render_color_chip(size_t category);
    void render_issue_row(size_t category, const wxString &title, const wxString &explanation,
                          float stats_col, float wrap_width);

    std::thread m_worker;
    std::mutex  m_state_mutex; // guards m_state
    State       m_state;       // accessed by both threads

    // UI-thread state
    std::optional<AnalysisKey> m_measure_key; // key m_measurement was computed for
    DfmMeasurement             m_measurement;
    DfmThresholds              m_thresholds;
    DfmClassification          m_classification;
    bool                       m_reclassify_pending       = false;
    bool                       m_rebuild_overlays_pending = false;

    std::vector<std::unique_ptr<VolumeOverlay>> m_overlays;
    std::array<bool, DfmCategoryCount>          m_category_visible{true, true, true, true};

    // Timestamp of the last rerender request. Only accessed from UI thread.
    int64_t m_last_rerender_timestamp = std::numeric_limits<int64_t>::min();

    // Input window geometry measured inside Begin/End on the previous frame: the height
    // feeds the bottom-limit clamp, the content height detects expand/collapse of the
    // tree nodes so an extra frame lets AlwaysAutoResize settle (no stale empty window).
    float m_last_input_window_height = 0.f;
    float m_last_content_height      = 0.f;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoDfmCheck_hpp_
