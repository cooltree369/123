#include "FileZilla.h"
#include "systemimagelist.h"

wxImageListEx::wxImageListEx()
	: wxImageList()
{
}

wxImageListEx::wxImageListEx(int width, int height, const bool mask /*=true*/, int initialCount /*=1*/)
	: wxImageList(width, height, mask, initialCount)
{
}

HIMAGELIST wxImageListEx::Detach()
{
	 HIMAGELIST hImageList = (HIMAGELIST)m_hImageList;
	 m_hImageList = 0;
	 return hImageList;
}

CSystemImageList::CSystemImageList(int size)
{
#ifdef __WXMSW__
	SHFILEINFO shFinfo;	
	int m_nStyle = 0;
	wxChar buffer[MAX_PATH + 10];
	if (!GetWindowsDirectory(buffer, MAX_PATH))
#ifdef _tcscpy
		_tcscpy(buffer, _T("C:\\"));
#else
		strcpy(buffer, _T("C:\\"));
#endif

	m_pImageList = new wxImageListEx((WXHIMAGELIST)SHGetFileInfo(buffer,
							  0,
							  &shFinfo,
							  sizeof( shFinfo ),
							  SHGFI_SYSICONINDEX |
							  ((size != 16) ? SHGFI_ICON : SHGFI_SMALLICON) ));
#else
	m_pImageList = new wxImageListEx(size, size);

	m_pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FILE"),  wxART_OTHER, wxSize(size, size)));
	m_pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FOLDER"),  wxART_OTHER, wxSize(size, size)));
#endif
}

CSystemImageList::~CSystemImageList()
{
	if (!m_pImageList)
		return;

#ifdef __WXMSW__
	m_pImageList->Detach();
#endif

	delete m_pImageList;

	m_pImageList = 0;
}

#ifndef __WXMSW__
// This function converts to the right size with the given background colour
wxBitmap PrepareIcon(wxIcon icon, wxColour colour)
{
	wxBitmap bmp(icon.GetWidth(), icon.GetHeight());
	wxMemoryDC dc;
	dc.SelectObject(bmp);
	dc.SetPen(wxPen(colour));
	dc.SetBrush(wxBrush(colour));
	dc.DrawRectangle(0, 0, icon.GetWidth(), icon.GetHeight());
	dc.DrawIcon(icon, 0, 0);
	dc.SelectObject(wxNullBitmap);
	
	wxImage img = bmp.ConvertToImage();
	img.SetMask();
	img.Rescale(16, 16);
	return img;
}
#endif

int CSystemImageList::GetIconIndex(bool dir, const wxString& fileName /*=_T("")*/, bool physical /*=true*/)
{
#ifdef __WXMSW__
	if (fileName == _T(""))
		physical = false;

	SHFILEINFO shFinfo;
	memset(&shFinfo, 0, sizeof(SHFILEINFO));
	if (SHGetFileInfo(fileName != _T("") ? fileName : _T("{B97D3074-1830-4b4a-9D8A-17A38B074052}"),
		dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
		&shFinfo,
		sizeof(SHFILEINFO),
		SHGFI_ICON | ((physical) ? 0 : SHGFI_USEFILEATTRIBUTES) ) )
	{
		int icon = shFinfo.iIcon;
		// we only need the index from the system image ctrl
		DestroyIcon(shFinfo.hIcon);
		return icon;
	}
#else
	int icon;
	if (dir)
		icon = 1;
	else
		icon = 0;

	wxFileName fn(fileName);
	wxString ext = fn.GetExt();

	wxFileType *pType = wxTheMimeTypesManager->GetFileTypeFromExtension(ext);
	if (pType)
	{
		wxIconLocation loc;
		if (pType->GetIcon(&loc) && loc.IsOk())
		{
			wxLogNull nul;
			wxIcon newIcon(loc);
				
			if (newIcon.Ok())
			{
				wxBitmap bmp = PrepareIcon(newIcon, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
				int index = m_pImageList->Add(bmp);
				if (index > 0)
					icon = index;
				bmp = PrepareIcon(newIcon, wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT));
				m_pImageList->Add(bmp);
			}
		}
		delete pType;
	}
	return icon;
#endif
	return -1;
}
