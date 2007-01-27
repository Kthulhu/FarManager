#ifndef __FILEFILTER_HPP__
#define __FILEFILTER_HPP__
/*
filefilter.hpp

�������� ������

*/

#include "plugin.hpp"
#include "struct.hpp"
#include "filefilterparams.hpp"

class VMenu;
class Panel;

enum enumFileFilterType {
  FFT_PANEL = 0,
  FFT_FINDFILE,
  FFT_COPY,
};

class FileFilter
{
  private:
    Panel *m_HostPanel;
    enumFileFilterType m_FilterType;

    int  ParseAndAddMasks(char **ExtPtr,const char *FileName,DWORD FileAttr,int& ExtCount,int Check);
    void ProcessSelection(VMenu *FilterList);
    void GetIncludeExcludeFlags(DWORD &Inc, DWORD &Exc);
    int  GetCheck(FileFilterParams *FFP);
    static void SwapPanelFlags(FileFilterParams *CurFilterData);

  public:
    FileFilter(Panel *HostPanel, enumFileFilterType FilterType);
    ~FileFilter();

    void FilterEdit();
    bool FileInFilter(WIN32_FIND_DATA *fd);
    bool IsEnabledOnPanel();

    static void InitFilter();
    static void CloseFilter();
    static void SwapFilter();
    static void SaveFilters(bool SaveAll=true);
};

#endif  // __FINDFILES_HPP__
