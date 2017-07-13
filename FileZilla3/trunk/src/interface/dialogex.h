#ifndef FILEZILLA_INTERFACE_DIALOGEX_HEADER
#define FILEZILLA_INTERFACE_DIALOGEX_HEADER

#include "wrapengine.h"

class wxDialogEx : public wxDialog, public CWrapEngine
{
public:
	bool Load(wxWindow *pParent, const wxString& name);

	bool SetChildLabel(int id, const wxString& label, unsigned long maxLength = 0);
	bool SetChildLabel(char const* id, const wxString& label, unsigned long maxLength = 0);
	wxString GetChildLabel(int id);

	virtual int ShowModal();

	bool ReplaceControl(wxWindow* old, wxWindow* wnd);

	static bool CanShowPopupDialog();
protected:
	virtual void InitDialog();

	DECLARE_EVENT_TABLE()
	virtual void OnChar(wxKeyEvent& event);
	void OnMenuEvent(wxCommandEvent& event);

#ifdef __WXMAC__
	virtual bool ProcessEvent(wxEvent& event);
#endif

	static int m_shown_dialogs;
};

#endif
