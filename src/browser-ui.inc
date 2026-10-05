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
void LayoutBrowserPanel(float w,float h){
 (void)w;if(panel!=PanelLibrary&&!(panel==PanelNone&&libraryPanelClosing&&panelSlide.v>.004f))return;
 auto body=D2D1::RectF(14-(1-Clamp(panelSlide.v,0,1))*24,8,14+SideWidth-(1-Clamp(panelSlide.v,0,1))*24,h-14);
 rects[IdPanelBody]=body;hots.push_back({IdPanelBody,body});
 Hotspot(IdLibMenu,D2D1::RectF(body.left+10,body.top+14,body.left+54,body.top+58));
 browserRows.clear();float y=body.top+96-panelScroll;
 auto row=[&](int id,const std::wstring& text,const wchar_t* glyph,const std::wstring& path,int indent=0,bool active=false,bool compact=false){
  float rowHeight=compact?32.f:44.f;
  D2D1_RECT_F r=D2D1::RectF(body.left+12+indent,y,body.right-12,y+rowHeight);y+=rowHeight+6;
  if(r.bottom<body.top+92||r.top>body.bottom-12)return;
  auto clipped=D2D1::RectF(r.left,(std::max)(body.top+92,r.top),r.right,(std::min)(body.bottom-12,r.bottom));
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
 panelExtent=(std::max)(0.f,y+panelScroll-(body.bottom-12));panelScroll=Clamp(panelScroll,0,panelExtent);
}
void PaintBrowserPanel(D2D1_RECT_F body,const Palette& p,float alpha){
 PaintMenuMorph(R(IdLibMenu),p,alpha);
 Write(language?L"FILES":L"ФАЙЛЫ",D2D1::RectF(body.left+68,body.top+15,body.right-18,body.top+57),F_Heading,Fade(p.text,alpha));
 Write(L"VETRO LOOK",D2D1::RectF(body.left+70,body.top+60,body.right-18,body.top+82),F_Section,Fade(p.dim,alpha));
 Dc()->PushAxisAlignedClip(D2D1::RectF(body.left+8,body.top+92,body.right-8,body.bottom-12),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
 for(auto& item:browserRows){if(item.rect.bottom<body.top+92||item.rect.top>body.bottom-12)continue;
  if(!item.id){Write(item.text,item.rect,F_Section,Fade(p.faint,alpha));continue;}
  if(item.active||(!springFolder.empty()&&item.path==springFolder)){Ink()->SetColor(Fade(p.accent,alpha*((!springFolder.empty()&&item.path==springFolder)?.22f:.12f)));Dc()->FillRoundedRectangle(D2D1::RoundedRect(item.rect,17,17),Ink());}
  PlateFor(item.id,item.rect,p,alpha,17);auto tint=item.active?p.accent:p.text;
  float inset=(Height(item.rect)-22)/2;
  Icon(item.glyph,D2D1::RectF(item.rect.left+12,item.rect.top+inset,item.rect.left+34,item.rect.bottom-inset),Fade(tint,alpha),1.7f);
  Write(Elide(item.text,F_Row,Width(item.rect)-66),D2D1::RectF(item.rect.left+44,item.rect.top,item.rect.right-18,item.rect.bottom),F_Row,Fade(tint,alpha));
  if(item.active&&!item.path.empty())Icon(IcChevronL,D2D1::RectF(item.rect.right-20,item.rect.top+inset+3,item.rect.right-4,item.rect.bottom-inset-3),Fade(p.accent,alpha),1.5f);
 }
 Dc()->PopAxisAlignedClip();
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
  if(x>body.left&&x<body.right){float before=panelScroll;if(y>body.bottom-36)panelScroll=Clamp(panelScroll+14,0,panelExtent);else if(y<body.top+120)panelScroll=Clamp(panelScroll-14,0,panelExtent);if(before!=panelScroll)Wake();}}
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
