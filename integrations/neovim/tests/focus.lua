-- Check focus and mode after the toggle mapping returns to the UI loop.
local child=vim.fn.jobstart({vim.v.progpath,'--embed','--headless','-u','NONE','-i','NONE'},{rpc=true})
assert(child>0,'Could not launch the focus fixture')
local function exec(code,...) return vim.rpcrequest(child,'nvim_exec_lua',code,{...}) end
local function input(keys) vim.rpcrequest(child,'nvim_input',keys) end
local function wait(predicate,message) assert(vim.wait(1000,predicate,10),message) end
local ok,err=xpcall(function()
  vim.rpcrequest(child,'nvim_ui_attach',80,24,{})
  exec([[
    local root,runtime=...
    vim.opt.runtimepath:prepend(runtime)
    focus_tabs={}
    for _,mode in ipairs({'n','i'}) do
      for _,key in ipairs({'<Tab>','<S-Tab>'}) do
        vim.keymap.set(mode,key,function()
          local id=mode..key
          focus_tabs[id]=(focus_tabs[id] or 0)+1
        end)
      end
    end
    local client={transport={state='ready',pending={}},conversation={},operations={},sent={}}
    focus_client=client
    function client:request(frame,terminal,callback)
      assert(frame.type=='mode.set' and terminal=='mode.updated','Unexpected daemon request')
      self.sent[#self.sent+1]={frame=frame,callback=callback}
      return tostring(#self.sent)
    end
    function focus_reply(mode,failure)
      local request=client.sent[#client.sent]
      local event={type=failure and 'request.failed' or 'mode.updated',mode=mode,error=failure}
      request.callback(event,failure)
      client.on_event(event)
    end
    focus_session=require('sos.session').new(client,{workspace=root,progress=false})
    focus_session.configured,focus_session.ever_connected=true,true
    focus_panel=require('sos.panel').new(focus_session)
    focus_origin=vim.api.nvim_get_current_win()
    local sos=require('sos')
    sos.session=function() return focus_session end
    sos.panel=function() return focus_panel end
    vim.cmd('runtime plugin/sos.lua')
    vim.keymap.set('n','<Space>o','<Cmd>SosToggle<CR>')
    focus_panel:show()
    assert(vim.api.nvim_get_current_win()==focus_origin,'Background show stole focus')
    assert(vim.api.nvim_win_get_height(focus_panel.input)==5,'Composer did not open with five lines')
    assert(vim.deep_equal(vim.api.nvim_buf_get_lines(focus_session.current.view.transcript,0,-1,false),{''}),
      'Empty transcript still shows a welcome label')
    focus_panel:hide()
  ]],vim.fn.getcwd(),vim.env.SOS_TEST_RUNTIME or vim.fn.getcwd()..'/integrations/neovim')
  local function opened()
    return exec('return focus_panel.input~=nil and vim.api.nvim_get_current_win()==focus_panel.input')
  end
  local function mode() return vim.rpcrequest(child,'nvim_get_mode').mode end
  input(' o')
  wait(function() return opened() and mode()=='n' end,'Toggle must focus the composer without entering Insert mode')
  assert(exec('return #focus_client.sent==0'),'Opening sent an unexpected request')
  input('<Tab><S-Tab>')
  wait(function() return exec("return #focus_client.sent==1 and focus_tabs['n<S-Tab>']==1") end,
    'Composer Tab did not request a mode change or replaced Shift-Tab')
  assert(exec("return focus_client.sent[1].frame.mode=='slop' and focus_session.current.mode=='sane' and not focus_tabs['n<Tab>']"),
    'Normal-mode Tab bypassed the daemon acknowledgement or reached the global mapping')
  exec("focus_reply('slop'); assert(vim.wo[focus_panel.input].winbar:find('SosModeSlop',1,true))")
  assert(opened() and mode()=='n','Normal-mode Tab changed chat focus or mode')
  input('iRetained draft')
  wait(function() return exec('return focus_session.current.draft')=='Retained draft' end,'Typing missed the composer')
  input('<Tab><S-Tab>')
  wait(function() return exec("return #focus_client.sent==2 and focus_tabs['i<S-Tab>']==1") end,
    'Insert-mode Tab did not request a mode change or replaced Shift-Tab')
  assert(exec("return focus_client.sent[2].frame.mode=='sane' and focus_session.current.mode=='slop' and not focus_tabs['i<Tab>']"),
    'Insert-mode Tab bypassed the daemon acknowledgement or reached completion')
  exec("focus_reply('sane'); assert(vim.wo[focus_panel.input].winbar:find('SosModeSane',1,true))")
  assert(opened() and mode()=='i','Insert-mode Tab changed chat focus or mode')
  assert(exec('return focus_session.current.draft')=='Retained draft','Mode switching changed the draft')
  input('<Tab>')
  wait(function() return exec('return #focus_client.sent==3') end,'Could not request a refused mode change')
  exec("focus_reply(nil,'Mode locked')")
  assert(exec("return focus_session.current.mode=='sane' and focus_session.current.notice=='Mode locked' and focus_session.current.draft=='Retained draft'"),
    'Refused mode change changed the mode or draft')
  assert(opened() and mode()=='i','Refused mode change changed focus or left Insert mode')
  input('<Esc>')
  wait(function() return mode()=='n' end,'Could not leave Insert mode')
  exec('vim.api.nvim_set_current_win(focus_panel.transcript)')
  input('<Tab><S-Tab>')
  wait(function() return exec("return focus_tabs['n<Tab>']==1 and focus_tabs['n<S-Tab>']==2") end,
    'Transcript did not inherit the user Tab mappings')
  assert(exec('return #focus_client.sent==3 and vim.api.nvim_get_current_win()==focus_panel.transcript'),
    'Transcript Tab switched focus or sent a mode request')
  input('i')
  wait(opened,'Transcript i did not focus the composer')
  exec("focus_session.current.kind='planning'; focus_session:changed()")
  input('<Tab>')
  wait(function() return exec("return focus_session.current.notice=='Mode changes are available only in editable Discussion'") end,
    'Planning composer allowed a mode change')
  assert(exec('return #focus_client.sent==3'),'Non-Discussion Tab sent a mode request')
  exec("focus_session.current.kind='discussion'")
  exec('focus_session.current.busy=true')
  input(' o')
  wait(function() return exec('return focus_panel.input==nil and focus_panel.transcript==nil') end,'Toggle did not hide the chat')
  assert(exec('return focus_session.current.busy'),'Hiding cancelled generation')
  assert(exec('return focus_session.current.draft')=='Retained draft','Hiding discarded the draft')
  assert(exec('return vim.api.nvim_get_current_win()==focus_origin'),'Hiding did not return to the file window')
  input(' o')
  wait(function() return opened() and mode()=='n' end,'Reopening changed the editing mode')
  assert(exec('return focus_session.current.draft')=='Retained draft','Reopening discarded the draft')
  input(' o')
  wait(function() return exec('return focus_panel.input==nil') end,'Could not hide the reopened panel')
  exec('focus_session.current.archived=true')
  input(' o')
  wait(function() return exec('return focus_panel.input==nil and vim.api.nvim_get_current_win()==focus_panel.transcript') end,
    'Read-only history did not focus its content without an unusable composer')
  assert(mode()=='n','Read-only history entered Insert mode')
  assert(exec('return not vim.bo[focus_session.current.view.input].modifiable'),'Locked composer became editable')
  assert(exec("return #focus_client.sent==3 and focus_session.current.mode=='sane' and focus_session.current.draft=='Retained draft'"),
    'Read-only layout changed mode, draft or sent a request')
  exec('focus_session.current.archived=false; focus_session:changed()')
  vim.rpcrequest(child,'nvim_ui_try_resize',80,10)
  exec([[
    local minimum=vim.o.winminheight
    focus_panel:hide(); focus_panel:show()
    local height=vim.api.nvim_win_get_height(focus_panel.input)
    assert(height>=1 and height<=5,'Initial composer height did not adapt to a short terminal')
    assert(vim.api.nvim_win_get_height(focus_panel.transcript)>=1,'Composer left no space for history')
    assert(vim.o.winminheight==minimum,'Initial sizing changed a global window option')
  ]])
end,debug.traceback)
vim.fn.jobstop(child)
assert(ok,err)
print('Composer Tab toggles Sane/Slop after acknowledgement; focus, drafts, Shift-Tab and locked contexts are preserved')
