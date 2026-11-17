-- Isolate native timestamp checks and the write-conflict prompt from other fixtures.
local root=vim.fn.getcwd()
vim.fn.mkdir(root..'/build-ai/neovim','p')
local work=assert(vim.uv.fs_mkdtemp(root..'/build-ai/neovim/refresh-XXXXXX'))
local child=vim.fn.jobstart({vim.v.progpath,'--embed','--headless','-u','NONE','-i','NONE'},{rpc=true})
assert(child>0,'Could not launch the file refresh fixture')
local function exec(code,...) return vim.rpcrequest(child,'nvim_exec_lua',code,{...}) end
local ok,err=xpcall(function()
  vim.rpcrequest(child,'nvim_ui_attach',100,30,{})
  exec([[
    local runtime,root=...
    vim.opt.runtimepath:prepend(runtime)
    local api=vim.api
    local function write(file,text)
      local fd=assert(vim.uv.fs_open(file,'w',384))
      assert(vim.uv.fs_write(fd,text,0)); vim.uv.fs_close(fd)
    end
    local function open(name)
      local file=root..'/'..name
      write(file,'original '..name..'\nkeep\n')
      local buffer=vim.fn.bufadd(file); vim.fn.bufload(buffer)
      return buffer,file
    end
    local function text(buffer) return table.concat(api.nvim_buf_get_lines(buffer,0,-1,false),'\n')..'\n' end
    local client={transport={state='ready',pending={}},conversation={},operations={},sent={}}
    function client:request(frame,terminal,callback)
      self.sent[#self.sent+1]={frame=frame,callback=callback}
      return tostring(#self.sent)
    end
    function client:reply(event)
      local request=self.sent[tonumber(event.requestId)]
      request.callback(event)
      self.on_event(event)
    end
    local s=require('sos.session').new(client,{workspace=root,progress=false})
    s.current.id='live'; s.configured=true
    s.workflow.attached_id='live'
    s.info={workspace=root}
    local checks={}
    local command=vim.cmd
    vim.cmd=function(value)
      if type(value)=='string' and value:match('^checktime') then checks[#checks+1]=value end
      return command(value)
    end
    local function settle() assert(vim.wait(1000,function() return not s.file_refresh.scheduled end,5)) end
    local serial=0
    local function result(metadata,kind,id)
      serial=serial+1
      s:event({type=kind or 'harness.message_arrived',sessionId=id or 'live',messages={
        {id='result-'..serial,role='tool',content={{type='tool_result',callId=tostring(serial),metadata=metadata}}}}})
    end
    local clean,file=open('file with spaces.txt')
    local unrelated,other=open('unrelated.txt')
    local dirty,dirty_path=open('dirty.txt')
    local disabled,disabled_path=open('noautoread.txt'); vim.bo[disabled].autoread=false
    local evidence,evidence_path=open('evidence.txt')
    s.inspector={buffers={[evidence]=true},update=function() end}
    local historical=api.nvim_create_buf(false,true)
    api.nvim_buf_set_name(historical,'sos-inspection://historical')
    api.nvim_buf_set_lines(historical,0,-1,false,{'immutable evidence'})
    vim.bo[historical].modifiable=false
    local absent=vim.fn.bufadd(root..'/not-loaded.txt')
    write(root..'/not-loaded.txt','not loaded\n')
    api.nvim_buf_set_lines(dirty,0,-1,false,{'unsaved editor changes'})
    local composer=api.nvim_create_buf(false,true)
    api.nvim_set_current_buf(composer)
    local focus=api.nvim_get_current_win()
    write(file,'agent write\nkeep\n'); write(other,'unrelated disk changes\n')
    write(dirty_path,'agent changes on disk\n'); write(disabled_path,'disabled disk changes\n')
    write(evidence_path,'successor must not replace evidence\n')
    local metadata={diff='patch',filepath=file}
    for _,kind in ipairs({'provider.message_arrived','session.history_chunk','session.attached','implementation.history','btw.finished','implementation.updated'}) do
      result(metadata,kind)
    end
    result(metadata,nil,'foreign')
    result({filepath=file,display='read output'})
    assert(not s.file_refresh.scheduled and #checks==0,'History, reads or foreign sessions triggered a reload')
    result(metadata); result(metadata)
    assert(text(clean):find('original',1,true),'Reload happened before the result reached the UI loop')
    settle()
    assert(text(clean)=='agent write\nkeep\n','Editing result did not reload the affected file')
    assert(#checks==1 and checks[1]=='checktime '..clean,'Editing checks were global or not coalesced')
    assert(text(unrelated):find('original',1,true),'Editing result checked an unrelated buffer')
    assert(api.nvim_get_current_win()==focus and api.nvim_get_current_buf()==composer,'Reload stole focus')

    -- Respect both inherited defaults and explicit buffer-local overrides.
    vim.go.autoread=false
    write(file,'local autoread override\n')
    result(metadata); settle()
    assert(#checks==1 and text(clean)=='agent write\nkeep\n','Reload ignored inherited noautoread')
    vim.bo[clean].autoread=true
    result(metadata); settle()
    assert(text(clean)=='local autoread override\n' and not vim.go.autoread,'Reload changed the global option or ignored a local override')
    vim.go.autoread=true

    -- Patch result metadata includes multiple paths and both ends of a move.
    local moved,moved_path=open('move destination.txt')
    write(moved_path,'moved destination contents\n')
    result({diff='multi-file patch',files={{filePath=file,movePath=moved_path},{filePath=dirty_path},
      {filePath=disabled_path},{filePath=evidence_path},{filePath=root..'/not-loaded.txt'}}})
    settle()
    assert(text(moved)=='moved destination contents\n')
    assert(text(dirty)=='unsaved editor changes\n' and vim.bo[dirty].modified,'Reload discarded a dirty buffer')
    assert(text(disabled):find('original',1,true) and not vim.bo[disabled].autoread,'Reload ignored noautoread')
    assert(text(evidence):find('original',1,true),'Reload replaced inspection-owned evidence')
    assert(text(historical)=='immutable evidence\n' and not api.nvim_buf_is_loaded(absent),'Reload opened a file or changed historical evidence')
    for _,check in ipairs(checks) do
      assert(check~='checktime '..dirty and check~='checktime '..disabled and check~='checktime '..evidence)
    end

    -- Absolute paths identify the real copy, not another file with the same basename.
    vim.fn.mkdir(root..'/isolated','p')
    local isolated,isolated_path=open('isolated/unrelated.txt')
    write(isolated_path,'isolated agent result\n')
    result({diff='isolated patch',filepath=isolated_path}); settle()
    assert(text(isolated)=='isolated agent result\n' and text(unrelated):find('original',1,true))

    local race,race_path=open('race.txt')
    write(race_path,'agent race result\n')
    local count=#checks
    result({diff='patch',filepath=race_path})
    api.nvim_buf_set_lines(race,0,-1,false,{'typed before scheduled check'})
    settle()
    assert(#checks==count and text(race)=='typed before scheduled check\n','Deferred check ignored a newly dirty buffer')
    local renamed,renamed_path=open('renamed-before-check.txt')
    write(renamed_path,'agent renamed result\n')
    result({diff='patch',filepath=renamed_path})
    api.nvim_buf_set_name(renamed,root..'/different-buffer-path.txt')
    settle(); assert(#checks==count,'Deferred check ignored a renamed buffer')
    local deleted,deleted_path=open('deleted.txt')
    assert(vim.uv.fs_unlink(deleted_path))
    result({diff='delete',files={{filePath=deleted_path,movePath=deleted_path}}})
    settle(); assert(#checks==count and text(deleted):find('original',1,true),'Deletion prompted or discarded the old contents')

    -- Terminal fallback covers shell/partial effects, bounded by workspace paths.
    local outside, outside_path=open('../'..vim.fn.fnamemodify(root,':t')..'-outside.txt')
    write(outside_path,'outside disk changes\n')
    for _,kind in ipairs({'execution.completed','execution.failed','execution.cancelled'}) do
      write(other,'shell result '..kind..'\n')
      s:event({type=kind,sessionId='live',info={workspace=root}}); settle()
      assert(text(unrelated)=='shell result '..kind..'\n','Terminal fallback did not reload shell effects')
      assert(text(dirty)=='unsaved editor changes\n' and text(evidence):find('original',1,true))
      assert(text(outside):find('original',1,true),'Terminal fallback escaped the workspace')
    end

    -- Only a confirmed whole-step import refreshes original-workspace buffers.
    local w=s.workflow
    w.branch_id='live'; w.info={workspace=root}
    w.state={revision=1,status='waiting',review_step=0,plan={steps={{title='Refresh',substeps={{title='Write'}}}}},steps={{identity='step',generation=1,
      substeps={{complete=true,reviewed=false,diffs={}}},whole={complete=true,reviewed=false,
        diffs={{path='file with spaces.txt'},{path='dirty.txt'}}}}}}
    local review=s:review_actions()
    local function submit(part,success)
      review:submit('implementation.validate',{step=0,substep=part},review:snapshot(0))
      local response=vim.deepcopy(w.state)
      if part==1 and success then response.steps[1].whole.reviewed=true end
      client:reply({type=success and 'implementation.updated' or 'request.failed',
        sessionId='live',requestId=tostring(#client.sent),implementation=response,
        error=not success and 'Import conflict' or nil})
      settle()
    end
    write(file,'imported original result\n'); write(other,'not part of this import\n')
    count=#checks
    submit(0,true); assert(#checks==count,'Review indicator triggered import refresh')
    submit(1,false); assert(#checks==count,'Refused import triggered refresh')
    submit(1,true)
    assert(#checks==count+1 and checks[#checks]=='checktime '..clean,'Import refreshed unrelated or dirty buffers')
    assert(text(clean)=='imported original result\n' and text(unrelated):find('shell result',1,true))

    -- Leave a newly dirty, currently focused file for a real interactive :write.
    -- No buffer switches or explicit timestamp checks may acknowledge its disk change.
    native_dirty,native_path=open('native-write-warning.txt')
    api.nvim_set_current_buf(native_dirty)
    api.nvim_buf_set_lines(native_dirty,0,-1,false,{'native unsaved editor text'})
    write(native_path,'external agent change that must not be overwritten\n')
    count=#checks
    result({diff='patch',filepath=native_path})
    settle()
    assert(#checks==count and vim.bo[native_dirty].modified,'Dirty timestamp was acknowledged')
    refresh_fixture={session=s,write=write,text=text,checks=checks}
    vim.cmd=command
  ]],vim.env.SOS_TEST_RUNTIME or root..'/integrations/neovim',work)
  vim.rpcrequest(child,'nvim_input',':write<CR>')
  local warned=vim.wait(1500,function() return vim.rpcrequest(child,'nvim_get_mode').mode=='r?' end,10)
  assert(warned,'Native write did not warn about the external change')
  vim.rpcrequest(child,'nvim_input','n')
  assert(vim.wait(1500,function() return vim.rpcrequest(child,'nvim_get_mode').mode~='r?' end,10),
    'Could not refuse the native write confirmation')
  if vim.rpcrequest(child,'nvim_get_mode').mode=='r' then vim.rpcrequest(child,'nvim_input','<CR>') end
  assert(vim.wait(1500,function() return vim.rpcrequest(child,'nvim_get_mode').mode=='n' end,10),
    'Could not dismiss the native write warning')
  assert(exec([[return table.concat(vim.fn.readfile(native_path),'\n')=='external agent change that must not be overwritten'
    and refresh_fixture.text(native_dirty)=='native unsaved editor text\n' and vim.bo[native_dirty].modified]]),
    'Native write warning lost the unsaved editor or external disk changes')
  exec([[
    local f=refresh_fixture
    f.insert_path=(...)..'/while-insert.txt'
    f.write(f.insert_path,'before insert\n')
    f.insert_buffer=vim.fn.bufadd(f.insert_path); vim.fn.bufload(f.insert_buffer)
    f.composer=vim.api.nvim_create_buf(false,true)
    vim.api.nvim_set_current_buf(f.composer)
  ]],work)
  vim.rpcrequest(child,'nvim_input','i')
  assert(vim.wait(1000,function() return vim.rpcrequest(child,'nvim_get_mode').mode=='i' end,10))
  exec([[
    local f=refresh_fixture
    f.write(f.insert_path,'reloaded during insertion\n')
    f.session:event({type='harness.message_arrived',sessionId='live',messages={{id='while-insert',role='tool',
      content={{type='tool_result',metadata={diff='patch',filepath=f.insert_path}}}}}})
  ]])
  assert(vim.wait(1500,function()
    return exec([[local f=refresh_fixture
      return not f.session.file_refresh.scheduled and f.text(f.insert_buffer)=='reloaded during insertion\n']])
  end,10),'File did not reload while typing in another buffer')
  assert(vim.rpcrequest(child,'nvim_get_mode').mode=='i'
    and exec('return vim.api.nvim_get_current_buf()==refresh_fixture.composer'),'Reload interrupted insertion or moved focus')
end,debug.traceback)
vim.fn.jobstop(child)
assert(ok,err)
print('Targeted native reloads, dirty-buffer write warnings, history/inspection isolation and confirmed-import refresh passed')
