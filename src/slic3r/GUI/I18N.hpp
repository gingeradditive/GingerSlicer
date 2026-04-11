#ifndef _
// i18n removed: English only. All macros pass through the input string unchanged.
#define _(s)    	Slic3r::GUI::I18N::translate((s))
#define _L(s)    	Slic3r::GUI::I18N::translate((s))
#define _devL(s)	wxString((s))
#define _omitL(s)   ("")
#define _utf8(s)    Slic3r::GUI::I18N::translate_utf8((s))
#define _u8L(s)     Slic3r::GUI::I18N::translate_utf8((s))
#endif /* _ */

#ifndef _CTX
#define _CTX(s, ctx) 	  Slic3r::GUI::I18N::translate((s))
#define _CTX_utf8(s, ctx) Slic3r::GUI::I18N::translate_utf8((s))
#endif /* _ */

#ifndef L
#define L(s) s
#endif /* L */

#ifndef L_CONTEXT
#define L_CONTEXT(s, context) s
#endif /* L */

#ifndef _CHB
#define _CHB(s) wxString(s, wxConvUTF8).utf8_str()
#endif /* _CHB */

#ifndef slic3r_GUI_I18N_hpp_
#define slic3r_GUI_I18N_hpp_

#include <wx/string.h>

namespace Slic3r { namespace GUI {

namespace I18N {
	inline wxString    translate(const char         *s)  { return wxString(s, wxConvUTF8); }
	inline wxString    translate(const wchar_t      *s)  { return wxString(s); }
	inline wxString    translate(const std::string  &s)  { return wxString(s.c_str(), wxConvUTF8); }
	inline wxString    translate(const std::wstring &s)  { return wxString(s.c_str()); }
	inline wxString    translate(const wxString     &s)  { return s; }

	inline wxString    translate(const char         *s, const char       *, unsigned int) { return wxString(s, wxConvUTF8); }
	inline wxString    translate(const wchar_t      *s, const wchar_t    *, unsigned int) { return wxString(s); }
	inline wxString    translate(const std::string  &s, const std::string &, unsigned int) { return wxString(s.c_str(), wxConvUTF8); }
	inline wxString    translate(const std::wstring &s, const std::wstring &, unsigned int) { return wxString(s.c_str()); }
	inline wxString    translate(const wxString     &s, const wxString     &, unsigned int) { return s; }

	inline std::string translate_utf8(const char         *s)  { return std::string(s); }
	inline std::string translate_utf8(const wchar_t      *s)  { return wxString(s).ToUTF8().data(); }
	inline std::string translate_utf8(const std::string  &s)  { return s; }
	inline std::string translate_utf8(const std::wstring &s)  { return wxString(s.c_str()).ToUTF8().data(); }
	inline std::string translate_utf8(const wxString     &s)  { return s.ToUTF8().data(); }

	inline std::string translate_utf8(const char         *s, const char       *, unsigned int) { return std::string(s); }
	inline std::string translate_utf8(const wchar_t      *s, const wchar_t    *, unsigned int) { return wxString(s).ToUTF8().data(); }
	inline std::string translate_utf8(const std::string  &s, const std::string &, unsigned int) { return s; }
	inline std::string translate_utf8(const std::wstring &s, const std::wstring &, unsigned int) { return wxString(s.c_str()).ToUTF8().data(); }
	inline std::string translate_utf8(const wxString     &s, const wxString     &, unsigned int) { return s.ToUTF8().data(); }

	inline wxString    translate(const char *s, const char*)         { return wxString(s, wxConvUTF8); }
	inline wxString    translate(const wchar_t *s, const char*)      { return wxString(s); }
	inline wxString    translate(const std::string &s, const char*)  { return wxString(s.c_str(), wxConvUTF8); }
	inline wxString    translate(const std::wstring &s, const char*) { return wxString(s.c_str()); }
	inline wxString    translate(const wxString &s, const char*)     { return s; }

	inline std::string translate_utf8(const char *s, const char*)         { return std::string(s); }
	inline std::string translate_utf8(const wchar_t *s, const char*)      { return wxString(s).ToUTF8().data(); }
	inline std::string translate_utf8(const std::string &s, const char*)  { return s; }
	inline std::string translate_utf8(const std::wstring &s, const char*) { return wxString(s.c_str()).ToUTF8().data(); }
	inline std::string translate_utf8(const wxString &s, const char*)     { return s.ToUTF8().data(); }
} // namespace I18N

// Return std::string as a wxString (no translation)
wxString	L_str(const std::string &str);

} // namespace GUI
} // namespace Slic3r

#ifndef _L_PLURAL
#define _L_PLURAL(s, plural, n) Slic3r::GUI::I18N::translate(s, plural, n)
#endif /* L */

#endif /* slic3r_GUI_I18N_hpp_ */
