// Part of main.cpp: the library uses the existing panel spring and glass renderer.
const wchar_t* FolderGlyph(FolderIcon kind){
 switch(kind){
 case FolderIcon::Pictures:return L"M4 5 L20 5 L20 19 L4 19 Z M6 17 L11 11 L15 15 L18 12 L20 14 M15 8 L16 8";
 case FolderIcon::Downloads:return L"M12 3 L12 15 M7 10 L12 15 L17 10 M4 16 L4 20 L20 20 L20 16";
 case FolderIcon::Desktop:return L"M3 4 L21 4 L21 16 L3 16 Z M12 16 L12 21 M8 21 L16 21";
 case FolderIcon::Documents:return L"M6 3 L14 3 L19 8 L19 21 L6 21 Z M14 3 L14 8 L19 8 M9 12 L16 12 M9 16 L16 16";
 case FolderIcon::Videos:return IcPlay;
 case FolderIcon::Music:return L"M10 18 L10 5 L20 3 L20 16 M10 5 L20 3 M10 18 C10 22 3 22 3 18 C3 15 10 15 10 18 M20 16 C20 20 13 20 13 16 C13 13 20 13 20 16";
 case FolderIcon::Drive:return L"M5 5 L19 5 L22 18 L2 18 Z M2 18 L2 21 L22 21 L22 18 M17 19.5 L19 19.5";
 default:return IcFolderIc;
 }
}
std::wstring FolderLabel(const BrowserFolder& folder){
 switch(folder.icon){
 case FolderIcon::Pictures:return language?L"Pictures":L"Изображения";
 case FolderIcon::Downloads:return language?L"Downloads":L"Загрузки";
 case FolderIcon::Desktop:return language?L"Desktop":L"Рабочий стол";
 case FolderIcon::Documents:return language?L"Documents":L"Документы";
 case FolderIcon::Videos:return language?L"Videos":L"Видео";
 case FolderIcon::Music:return language?L"Music":L"Музыка";
 default:return folder.name;
 }
}
void PaintMenuMorph(D2D1_RECT_F rect,const Palette& p,float alpha){
 float t=Clamp(panelSlide.v,0,1),cx=(rect.left+rect.right)/2,cy=(rect.top+rect.bottom)/2;
 Ink()->SetColor(Fade(p.accent,alpha));
 Dc()->DrawLine(D2D1::Point2F(cx-10,cy-6+16*t),D2D1::Point2F(cx+10,cy-6-4*t),Ink(),2.3f);
 Ink()->SetColor(Fade(p.accent,alpha*(1-t)));Dc()->DrawLine(D2D1::Point2F(cx-10,cy),D2D1::Point2F(cx+10,cy),Ink(),2.3f);
 Ink()->SetColor(Fade(p.accent,alpha));Dc()->DrawLine(D2D1::Point2F(cx-10,cy+6-16*t),D2D1::Point2F(cx+10,cy+6+4*t),Ink(),2.3f);
}
void NavigateFolder(const std::wstring& path){
 libSearch.clear();favouritesOpen=false;albumFolder=path;screen=ScrAlbum;
 albumScroll=albumScrollVel=0;selectedPhotos.clear();folderTx.active=hero.active=false;
 browserPath=path;browserChildren.clear();browserLoading=true;browserError.clear();
 browserGeneration=BrowserRequest(path);IndexTouchFolder(path);RefreshAlbum();Wake();
}
void NavigateRecent(){
 browserPath.clear();browserChildren.clear();libSearch.clear();navStack.clear();
 albumFolder.clear();favouritesOpen=false;screen=ScrLibrary;libView=ViewPhotosFlat;
 libScroll=libScrollVel=0;RefreshLibrary();Wake();
}
// Settings sit at the very foot of the panel. The header never moves; its three
// rows open upward out of it and shorten the folder list rather than floating
// over it, so nothing being browsed is ever covered.
constexpr float SettingsHeaderH=44,SettingsItemH=50,SettingsItemGap=6;
constexpr float SettingsItemsH=3*SettingsItemH+2*SettingsItemGap;
float libListBottom=0;                    // where the folder list ends this frame
D2D1_RECT_F libSettingsClip{};            // the window the opening rows are seen through
const wchar_t* const MicGlyph=L"M12 3 C13.7 3 15 4.3 15 6 L15 11 C15 12.7 13.7 14 12 14 C10.3 14 9 12.7 9 11 L9 6 C9 4.3 10.3 3 12 3 Z M6 11 C6 14.3 8.7 17 12 17 C15.3 17 18 14.3 18 11 M12 17 L12 21 M8.5 21 L15.5 21";
std::wstring ModelSizeText(uint64_t bytes){
 wchar_t text[32];
 if(bytes>=1000ull*1048576ull)swprintf_s(text,language?L"%.1f GB":L"%.1f ГБ",double(bytes)/1073741824.0);
 else swprintf_s(text,language?L"%.0f MB":L"%.0f МБ",double(bytes)/1048576.0);
 return text;
}
D2D1_RECT_F SettingsClipTo(D2D1_RECT_F r,D2D1_RECT_F window){
 return D2D1::RectF((std::max)(r.left,window.left),(std::max)(r.top,window.top),(std::min)(r.right,window.right),(std::min)(r.bottom,window.bottom));
}
void LayoutBrowserPanel(float w,float h){
 (void)w;if(panel!=PanelLibrary&&!(panel==PanelNone&&libraryPanelClosing&&panelSlide.v>.004f))return;
 auto body=D2D1::RectF(14-(1-Clamp(panelSlide.v,0,1))*24,8,14+SideWidth-(1-Clamp(panelSlide.v,0,1))*24,h-14);
 rects[IdPanelBody]=body;hots.push_back({IdPanelBody,body});
 Hotspot(IdLibMenu,D2D1::RectF(body.left+10,body.top+14,body.left+54,body.top+58));
 // The settings footer is placed first because the list above it ends where it begins.
 float open=Clamp(settingsSlide.v,0,1);
 auto header=D2D1::RectF(body.left+12,body.bottom-12-SettingsHeaderH,body.right-12,body.bottom-12);
 Hotspot(IdLibSettings,header);
 float revealed=(std::min)(SettingsItemsH*open,(std::max)(0.f,Height(body)-250.f));
 libListBottom=header.top-12-revealed;
 libSettingsClip=D2D1::RectF(body.left+4,header.top-SettingsItemGap-revealed,body.right-4,header.top-SettingsItemGap);
 if(open>.004f){
  // Controls only answer once the rows have come to rest, so a click cannot
  // land on a row that is still sliding out from under the header.
  bool live=open>.9f;
  auto control=[&](int id,D2D1_RECT_F r){
   rects[id]=r;
   if(live){auto seen=SettingsClipTo(r,libSettingsClip);if(Height(seen)>6)hots.push_back({id,seen});}
  };
  auto itemAt=[&](int index){
   float top=libSettingsClip.top+index*(SettingsItemH+SettingsItemGap);
   return D2D1::RectF(body.left+12,top,body.right-12,top+SettingsItemH);
  };
  // The selector sits to the right of its translated label, as in the viewer menu.
  auto segments=[&](int rowId,int leftId,int rightId,D2D1_RECT_F row,const wchar_t* label){
   rects[rowId]=row;
   float labelRight=row.left+56+Measure(label,F_Row,120)+12;
   float half=Clamp((row.right-8-labelRight-4)*.5f,76.f,92.f);
   float left=(std::max)(labelRight,row.right-8-(half*2+4));
   control(leftId,D2D1::RectF(left,row.top+9,left+half,row.bottom-9));
   control(rightId,D2D1::RectF(left+half+4,row.top+9,left+half*2+4,row.bottom-9));
  };
  segments(IdLibThemeRow,IdThemeDark,IdThemeLight,itemAt(0),T(S_ThemeLabel));
  segments(IdLibLangRow,IdLangRu,IdLangEn,itemAt(1),T(S_Language));
  control(IdLibSpeechRow,itemAt(2));
 }
 browserRows.clear();float y=body.top+96-panelScroll;
 auto row=[&](int id,const std::wstring& text,const wchar_t* glyph,const std::wstring& path,int indent=0,bool active=false,bool compact=false){
  float rowHeight=compact?32.f:44.f;
  D2D1_RECT_F r=D2D1::RectF(body.left+12+indent,y,body.right-12,y+rowHeight);y+=rowHeight+6;
  if(r.bottom<body.top+92||r.top>libListBottom)return;
  auto clipped=D2D1::RectF(r.left,(std::max)(body.top+92,r.top),r.right,(std::min)(libListBottom,r.bottom));
  rects[id]=r;if(Height(clipped)>0)hots.push_back({id,clipped});
  browserRows.push_back({id,r,text,glyph,path,active});
 };
 auto section=[&](const wchar_t* text){y+=12;browserRows.push_back({0,D2D1::RectF(body.left+24,y,body.right-20,y+22),text,nullptr,L"",false});y+=30;};
 row(IdNavRecent,language?L"Recent":L"Недавние",L"M12 3 C17 3 21 7 21 12 C21 17 17 21 12 21 C7 21 3 17 3 12 C3 7 7 3 12 3 Z M12 7 L12 12 L16 14",L"",0,screen==ScrLibrary);
 row(IdNavFavourites,T(S_Favourites),IcHeart,L"",0,favouritesOpen);
 int slot=0;auto pins=BrowserPins();if(!pins.empty()){section(language?L"PINNED":L"ЗАКРЕПЛЁННЫЕ");for(auto& f:pins)row(IdNavFolder0+slot++,f.name,FolderGlyph(f.icon),f.path,0,BrowserWithin(browserPath,f.path));}
 if(!browserPath.empty()){
  section(language?L"CURRENT PATH":L"ТЕКУЩИЙ ПУТЬ");auto stack=BrowserAncestry(browserPath);
  // Cap indentation, not the path: deeply nested folders stay usable at any depth.
  for(size_t i=0;i<stack.size();i++)row(IdNavFolder0+slot++,FolderLabel(stack[i]),FolderGlyph(stack[i].icon),stack[i].path,int((std::min)(i,size_t(8)))*12,i+1==stack.size(),true);
  section(language?L"FOLDERS":L"ПАПКИ");
  if(browserLoading)browserRows.push_back({0,D2D1::RectF(body.left+24,y,body.right-20,y+28),T(S_Reading),nullptr,L"",false});
  else if(!browserError.empty())browserRows.push_back({0,D2D1::RectF(body.left+24,y,body.right-20,y+28),language?L"Folder unavailable":L"Папка недоступна",nullptr,L"",false});
  else if(browserChildren.empty())browserRows.push_back({0,D2D1::RectF(body.left+24,y,body.right-20,y+28),language?L"No subfolders":L"Нет вложенных папок",nullptr,L"",false});
  for(auto& f:browserChildren)row(IdNavFolder0+slot++,f.name,IcFolderIc,f.path);
 }
 section(language?L"LOCATIONS":L"РАСПОЛОЖЕНИЯ");
 for(auto& f:BrowserRoots())row(IdNavFolder0+slot++,FolderLabel(f),FolderGlyph(f.icon),f.path,0,NormalisePath(browserPath)==NormalisePath(f.path));
 panelExtent=(std::max)(0.f,y+panelScroll-libListBottom);panelScroll=Clamp(panelScroll,0,panelExtent);
}
// The speech-recognition model is a large download the viewer chooses, so the
// row says what it is doing and what its one tap will do: fetch it, stop the
// fetch, or take it back. Same three states as the viewer menu's AI row.
void PaintSpeechRow(const Palette& p,float alpha){
 auto r=R(IdLibSpeechRow);
 if(Width(r)<=0)return;
 auto ai=AiStatusNow();
 bool fetching=ai.state==AiState::Downloading,ready=AiInstalled();
 std::wstring status,action;
 if(fetching){
  wchar_t percent[16];swprintf_s(percent,L"%.0f%%",ai.downloadFraction*100);
  status=std::wstring(language?L"Downloading… ":L"Скачивание… ")+percent;action=language?L"Cancel":L"Отмена";
 }else if(ready){
  status=std::wstring(language?L"Ready · ":L"Готово · ")+ModelSizeText(AiInstalledBytes());action=language?L"Remove":L"Удалить";
 }else{
  status=std::wstring(language?L"Not downloaded · ":L"Не скачано · ")+ModelSizeText(AiDownloadBytes());action=language?L"Download":L"Скачать";
 }
 D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
 Dc()->SetTransform(Lift(r,IdLibSpeechRow)*Mat(previous));
 auto card=D2D1::RoundedRect(r,RowRadius,RowRadius);
 Ink()->SetColor(Fade(p.card,alpha));Dc()->FillRoundedRectangle(card,Ink());
 PlateFor(IdLibSpeechRow,r,p,alpha,RowRadius);
 Ink()->SetColor(Fade(p.cardEdge,alpha*.8f));Dc()->DrawRoundedRectangle(card,Ink(),1.f);
 float mid=(r.top+r.bottom)/2;
 Icon(MicGlyph,D2D1::RectF(r.left+16,mid-13,r.left+42,mid+13),Fade(p.text,alpha),1.75f);
 float actionLeft=r.right-18-(std::max)(56.f,Measure(action,F_Value,120)+4);
 Write(Elide(language?L"Speech recognition":L"Распознавание речи",F_Row,actionLeft-8-(r.left+56)),
  D2D1::RectF(r.left+56,r.top+6,actionLeft-8,mid+1),F_Row,Fade(p.text,alpha));
 Write(Elide(status,F_Label,actionLeft-8-(r.left+56)),D2D1::RectF(r.left+56,mid,actionLeft-8,r.bottom-8),F_Label,Fade(p.dim,alpha));
 Write(action,D2D1::RectF(actionLeft,r.top,r.right-18,r.bottom),F_Value,Fade(fetching?p.dim:p.accent,alpha));
 if(fetching){
  float left=r.left+56,right=r.right-18,bar=r.bottom-6,fraction=Clamp(float(ai.downloadFraction),0,1);
  Ink()->SetColor(Fade(p.faint,alpha*.45f));
  Dc()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,bar-1.5f,right,bar+1.5f),1.5f,1.5f),Ink());
  Ink()->SetColor(Fade(p.text,alpha));
  Dc()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,bar-1.5f,(std::max)(left+3,left+(right-left)*fraction),bar+1.5f),1.5f,1.5f),Ink());
 }
 Dc()->SetTransform(previous);
}
void PaintBrowserFooter(D2D1_RECT_F body,const Palette& p,float alpha){
 float open=Clamp(settingsSlide.v,0,1);
 float line=libListBottom+6;
 Ink()->SetColor(Fade(p.sep,alpha));
 Dc()->DrawLine(D2D1::Point2F(body.left+24,line),D2D1::Point2F(body.right-24,line),Ink(),1.f);
 PanelRow(IdLibSettings,IcSettings,language?L"Settings":L"Настройки",p,alpha,false,open>.5f?p.accent:p.text);
 auto header=R(IdLibSettings);
 if(Width(header)>0){
  // Points right when closed and up when open, the way the rows will move.
  D2D1_MATRIX_3X2_F previous;Dc()->GetTransform(&previous);
  Dc()->SetTransform(Lift(header,IdLibSettings)*Mat(previous));
  float mid=(header.top+header.bottom)/2;
  Icon(IcChevron,D2D1::RectF(header.right-32,mid-9,header.right-14,mid+9),Fade(p.faint,alpha),1.6f,false,-90.f*open);
  Dc()->SetTransform(previous);
 }
 if(open<=.004f)return;
 float shown=alpha*open;
 Dc()->PushAxisAlignedClip(libSettingsClip,D2D1_ANTIALIAS_MODE_ALIASED);
 SegmentedRow(IdLibThemeRow,IdThemeDark,IdThemeLight,themeMix.v>.5f?IcSun:IcMoon,T(S_ThemeLabel),T(S_Dark),T(S_Light),themeMix.v,p,shown);
 SegmentedRow(IdLibLangRow,IdLangRu,IdLangEn,IcGlobe,T(S_Language),L"Русский",L"English",langMix.v,p,shown);
 PaintSpeechRow(p,shown);
 Dc()->PopAxisAlignedClip();
}
void PaintBrowserPanel(D2D1_RECT_F body,const Palette& p,float alpha){
 PaintMenuMorph(R(IdLibMenu),p,alpha);
 Write(language?L"FILES":L"ФАЙЛЫ",D2D1::RectF(body.left+68,body.top+15,body.right-18,body.top+57),F_Heading,Fade(p.text,alpha));
 Write(L"VETRO LOOK",D2D1::RectF(body.left+70,body.top+60,body.right-18,body.top+82),F_Section,Fade(p.dim,alpha));
 Dc()->PushAxisAlignedClip(D2D1::RectF(body.left+8,body.top+92,body.right-8,libListBottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
 for(auto& item:browserRows){if(item.rect.bottom<body.top+92||item.rect.top>libListBottom)continue;
  if(!item.id){Write(item.text,item.rect,F_Section,Fade(p.faint,alpha));continue;}
  if(item.active||(!springFolder.empty()&&item.path==springFolder)){Ink()->SetColor(Fade(p.accent,alpha*((!springFolder.empty()&&item.path==springFolder)?.22f:.12f)));Dc()->FillRoundedRectangle(D2D1::RoundedRect(item.rect,17,17),Ink());}
  PlateFor(item.id,item.rect,p,alpha,17);auto tint=item.active?p.accent:p.text;
  float inset=(Height(item.rect)-22)/2;
  Icon(item.glyph,D2D1::RectF(item.rect.left+12,item.rect.top+inset,item.rect.left+34,item.rect.bottom-inset),Fade(tint,alpha),1.7f);
  Write(Elide(item.text,F_Row,Width(item.rect)-66),D2D1::RectF(item.rect.left+44,item.rect.top,item.rect.right-18,item.rect.bottom),F_Row,Fade(tint,alpha));
  if(item.active&&!item.path.empty())Icon(IcChevronL,D2D1::RectF(item.rect.right-20,item.rect.top+inset+3,item.rect.right-4,item.rect.bottom-inset-3),Fade(p.accent,alpha),1.5f);
 }
 Dc()->PopAxisAlignedClip();
 PaintBrowserFooter(body,p,alpha);
}
std::wstring BrowserFolderAt(float x,float y){
 int id=HitTest(x,y);for(auto& r:browserRows)if(r.id==id&&!r.path.empty())return r.path;
 if(screen==ScrLibrary&&libView==ViewFolders&&id>=IdCard0&&size_t(id-IdCard0)<libFolders.size())return libFolders[id-IdCard0].path;
 if(screen==ScrAlbum&&!favouritesOpen&&id==IdNone)return albumFolder;
 return {};
}
bool SpringFolderHover(POINT point){
 if(screen==ScrViewer)return true;float x=point.x/dpi,y=point.y/dpi;dropPoint=point;fileDropActive=true;SetTimer(win,12,50,nullptr);
 if(HitTest(x,y)==IdLibMenu&&panel!=PanelLibrary){libraryPanelClosing=true;OpenPanel(PanelLibrary);}
 auto folder=BrowserFolderAt(x,y);
 if(folder!=springFolder){springFolder=folder;springBegan=GetTickCount64();springOpened=false;Wake();}
 return !folder.empty()||HitTest(x,y)==IdNone||screen==ScrViewer;
}
void SpringFolderTick(){
 if(!fileDropActive)return;
 if(panel==PanelLibrary){auto body=R(IdPanelBody);float x=dropPoint.x/dpi,y=dropPoint.y/dpi;
  if(x>body.left&&x<body.right){float before=panelScroll;if(y>libListBottom-30)panelScroll=Clamp(panelScroll+14,0,panelExtent);else if(y<body.top+120)panelScroll=Clamp(panelScroll-14,0,panelExtent);if(before!=panelScroll)Wake();}}
 auto folder=BrowserFolderAt(dropPoint.x/dpi,dropPoint.y/dpi);
 if(folder!=springFolder){springFolder=folder;springBegan=GetTickCount64();springOpened=false;Wake();}
 if(!springFolder.empty()&&!springOpened&&GetTickCount64()-springBegan>=500){springOpened=true;auto path=springFolder;NavigateFolder(path);panelScroll=0;}
}
void StopSpringFolders(){fileDropActive=false;springFolder.clear();KillTimer(win,12);Wake();}
void BrowserContextMenu(float x,float y){
 auto path=BrowserFolderAt(x,y);if(path.empty())return;
 HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,BrowserIsPinned(path)?(language?L"Unpin":L"Открепить"):(language?L"Pin":L"Закрепить"));
 AppendMenuW(menu,MF_STRING|(BrowserCanDelete(path)?0:MF_GRAYED),2,language?L"Delete to Recycle Bin":L"Удалить в корзину");
 POINT point{LONG(x*dpi),LONG(y*dpi)};ClientToScreen(win,&point);int chosen=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,win,nullptr);DestroyMenu(menu);
 if(chosen==1){BrowserTogglePin(path);Wake();}
 else if(chosen==2&&BrowserRecycle(win,path)){if(BrowserIsPinned(path))BrowserTogglePin(path);auto parent=fs::path(path).parent_path().wstring();NavigateFolder(parent);IndexRescan();}
}
