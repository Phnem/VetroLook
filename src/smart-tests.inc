// Headless integration checks of the real worker, thumbnail cache and disk records.
int RunSmartIntegration(const std::wstring& log,const std::wstring& model,const std::wstring& folder,const std::wstring& cache,const std::wstring& replacement){
 std::wofstream out{fs::path(log)};int failures=0;
 auto check=[&](bool ok,const wchar_t* text){out<<(ok?L"PASS ":L"FAIL ")<<text<<L"\n";out.flush();if(!ok)failures++;};
 std::vector<PhotoEntry> photos;std::error_code ec;
 for(fs::recursive_directory_iterator it(folder,fs::directory_options::skip_permission_denied,ec),end;it!=end&&!ec;it.increment(ec))if(it->is_directory(ec)){FolderEntry f;auto part=IndexInspectDirectory(it->path().wstring(),f);photos.insert(photos.end(),part.begin(),part.end());}
 FolderEntry root;auto direct=IndexInspectDirectory(folder,root);photos.insert(photos.end(),direct.begin(),direct.end());
 check(!photos.empty(),L"fixture files indexed");if(photos.empty())return 1;
 ThumbStart(nullptr,0);SmartTestOptions options{cache,model};
 auto waitReady=[&]{auto start=GetTickCount64();while(GetTickCount64()-start<30000){auto s=SmartStatusNow();if(s.modelReady)return true;if(!s.error.empty())return false;Sleep(10);}return false;};
 auto waitRecords=[&]{auto start=GetTickCount64();while(GetTickCount64()-start<30000){bool ready=true;for(auto& p:photos)ready=ready&&SmartLookup(p).algorithm!=0;if(ready)return true;Sleep(10);}return false;};
 SmartStart(nullptr,0,&options);check(waitReady(),L"native TinyNeXt WinML session initialized");
 SmartPause(true);auto completed=SmartStatusNow().completed;auto stalePhotos=photos;stalePhotos.front().modified--;SmartRequest(stalePhotos);SmartSyncLibrary(photos,true);Sleep(80);check(SmartStatusNow().completed==completed,L"classification pauses without processing queued files");SmartPause(false);
 check(waitRecords(),L"gallery metadata -> existing thumbnail -> model -> decision pipeline");
 check(SmartStatusNow().completed>photos.size(),L"file edited while queued is retried with its latest inventory stamp");
 check(SmartStatusNow().reviewReady&&SmartStatusNow().reviewCompleted==photos.size()&&!SmartStatusNow().autoApply,L"first batch completes without applying labels");
 SmartSettings staged;staged.applyAutomatic=false;bool kept=true;for(auto& p:photos)kept=kept&&SmartPasses(p,staged);check(kept,L"staged classification leaves the whole gallery visible");
 unsigned visible=0,hidden=0,uncertain=0;for(auto& p:photos){auto r=SmartLookup(p);if(r.visibility==SmartVisibility::Visible)visible++;else if(r.visibility==SmartVisibility::Hidden)hidden++;else uncertain++;}
 out<<L"counts visible="<<visible<<L" hidden="<<hidden<<L" uncertain="<<uncertain<<L"\n";
 auto chosen=photos.front();auto before=SmartLookup(chosen);check(before.algorithm!=0&&before.modelVersion!=0,L"versioned decisions are recorded");
 SmartSetOverride(chosen,0);check(!SmartPasses(chosen,SmartSettings{}),L"manual Hide beats the classifier");check(SmartPasses(chosen,SmartSettings{false,63,false}),L"Show all reveals manually hidden files for correction");
 SmartSetOverride(chosen,1);check(SmartPasses(chosen,SmartSettings{true,0,false}),L"manual Show beats category filters");check(SmartApplyReviewed(),L"completed first batch can be applied");SmartStop();
 SmartStart(nullptr,0,&options);check(waitReady(),L"model restart");check(SmartStatusNow().autoApply,L"applied choice survives real worker restart");check(SmartLookup(chosen).overrideValue==1,L"manual corrections survive worker restart and disk reload");
 SmartRequest(photos);Sleep(200);check(SmartStatusNow().completed==0,L"valid cache avoids repeated inference after restart");
 auto changed=chosen;changed.modified++;check(SmartLookup(changed).algorithm==0&&SmartLookup(changed).visibility==SmartVisibility::Uncertain&&SmartLookup(changed).overrideValue==1,L"file changes invalidate decisions while preserving manual correction");
 SmartStop();options.modelPath=replacement;SmartStart(nullptr,0,&options);check(waitReady(),L"replacement model initializes");check(!SmartStatusNow().autoApply,L"replacement model does not inherit the application choice");SmartRequest(photos);
 auto began=GetTickCount64();while(GetTickCount64()-began<30000&&SmartStatusNow().completed<photos.size())Sleep(10);
 auto replaced=SmartLookup(chosen);check(replaced.modelVersion!=before.modelVersion&&replaced.overrideValue==1,L"changing model bytes triggers reclassification and retains overrides");
 SmartStop();ThumbStop();out<<L"DONE failures="<<failures<<L"\n";return failures?1:0;
}
