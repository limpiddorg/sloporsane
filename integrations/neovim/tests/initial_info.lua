-- Launched by smoke.lua after saving defaults in an isolated real daemon.
vim.opt.runtimepath:prepend(vim.env.SOS_TEST_RUNTIME or vim.fn.getcwd()..'/integrations/neovim')
local ok,err=xpcall(function()
  vim.o.columns=200
  vim.o.lines=40
  vim.cmd('runtime plugin/sos.lua')
  local sos=require('sos')
  sos.setup({socket=vim.env.SOS_TEST_SOCKET,auto_start=false,timeout_ms=5000,
    workspace=vim.env.SOS_TEST_WORKSPACE,progress=false})
  local client=sos.client()
  local session=sos.session()
  local panel=sos.panel()
  local sent={}
  local request=client.request
  client.request=function(self,frame,...)
    sent[#sent+1]=vim.deepcopy(frame)
    return request(self,frame,...)
  end
  assert(client.transport.state=='disconnected' and #sent==0,'Plugin loading connected eagerly')
  session:set_draft('Unsent draft')
  local function wait(predicate,message) assert(vim.wait(10000,predicate,10),message) end
  local function header()
    return vim.api.nvim_eval_statusline(vim.wo[panel.input].winbar,
      {winid=panel.input,maxwidth=vim.api.nvim_win_get_width(panel.input)}).str
  end
  vim.cmd('SosToggle')
  wait(function() return session.configured and not panel.render_pending end,'First opening did not initialize saved defaults')
  assert(session.info.model=='local-model' and session.info.context_limit==65536 and session.info.total_context==65536)
  assert(header():find('local-model',1,true),header())
  assert(header():find('— / 65 536 [—]',1,true),header())
  assert(not session.current.id and not client.conversation.session_id and #session.current.records==0,
    'Opening created a session or message')
  assert(session.current.draft=='Unsent draft','Initialization consumed the draft')
  assert(#sent==1 and sent[1].type=='integration.configure','Initialization dispatched more than configuration')
  assert(vim.api.nvim_get_current_win()==panel.input and vim.fn.mode()=='n','Initialization changed focus or mode')
  vim.cmd('SosToggle')
  vim.cmd('SosToggle')
  assert(#sent==1,'Reopening reconfigured an existing connection')
  client:close()
  wait(function() return client.transport.state=='disconnected' and not session.configured end,'Could not close the fixture connection')
  vim.cmd('SosToggle')
  vim.cmd('SosToggle')
  assert(client.transport.state=='disconnected' and #sent==1,'Toggle silently reconnected after disconnect')
  vim.cmd('SosReconnect')
  wait(function() return session.configured and client.synchronized and not panel.render_pending end,'Explicit reconnect did not initialize the idle integration')
  assert(not session.current.id and session.current.draft=='Unsent draft' and header():find('65 536',1,true))
  assert(#sent==2 and sent[2].type=='integration.configure','Reconnect sent an unexpected operation')
  panel:hide()
  client:close()
end,debug.traceback)
if not ok then io.stderr:write(err..'\n'); vim.cmd('cquit 1') end
print('First opening restores saved model/context without a session; focus, draft and explicit reconnect are preserved')
vim.cmd('qa!')
