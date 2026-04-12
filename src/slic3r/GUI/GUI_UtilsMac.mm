#include <unistd.h>
#include <sys/sysctl.h>
#import <wx/osx/cocoa/dataview.h>
#import "GUI_Utils.hpp"

namespace Slic3r {
namespace GUI {

void dataview_remove_insets(wxDataViewCtrl* dv) {
    NSScrollView* scrollview = (NSScrollView*) ((wxCocoaDataViewControl*)dv->GetDataViewPeer())->GetWXWidget();
    NSOutlineView* outlineview = scrollview.documentView;
    [outlineview setIntercellSpacing: NSMakeSize(0.0, 1.0)];
    if (@available(macOS 11, *)) {
        [outlineview setStyle:NSTableViewStylePlain];
    }
}

void staticbox_remove_margin(wxStaticBox* sb) {
    NSBox* nativeBox = (NSBox*)sb->GetHandle();
    [nativeBox setBoxType:NSBoxCustom];
    [nativeBox setBorderWidth:0];
}

float mac_max_scaling_factor() {
    // Get the maximum scaling factor across all screens
    float max_scale = 1.0f;
    NSArray *screens = [NSScreen screens];
    for (NSScreen *screen in screens) {
        float scale = screen.backingScaleFactor;
        if (scale > max_scale) {
            max_scale = scale;
        }
    }
    return max_scale;
}

bool is_debugger_present()
// Returns true if the current process is being debugged (either
// running under the debugger or has a debugger attached post facto).
// https://stackoverflow.com/a/2200786/3289421
{
    int                 junk;
    int                 mib[4];
    struct kinfo_proc   info;
    size_t              size;

    // Initialize the flags so that, if sysctl fails for some bizarre
    // reason, we get a predictable result.

    info.kp_proc.p_flag = 0;

    // Initialize mib, which tells sysctl the info we want, in this case
    // we're looking for information about a specific process ID.

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    // Call sysctl.

    size = sizeof(info);
    junk = sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);
    assert(junk == 0);

    // We're being debugged if the P_TRACED flag is set.

    return ( (info.kp_proc.p_flag & P_TRACED) != 0 );
}

void set_miniaturizable(WXWidget widget)
{
    NSView* view = (NSView*)widget;
    NSWindow* window = [view window];
    if (window) {
        [window setMiniaturizable:YES];
    }
}

void set_title_colour_after_set_title(WXWidget widget)
{
    // This function is a no-op on modern macOS
    // Title bar color is handled by the system
}

void set_tag_when_enter_full_screen(bool is_full_screen)
{
    // This function is a no-op
    // Tagging behavior is handled by the system
}

}
}

