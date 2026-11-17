-- Launched by smoke.py with isolated XDG paths and a local provider fixture.
vim.opt.runtimepath:prepend(vim.env.SOS_TEST_RUNTIME or vim.fn.getcwd() .. '/integrations/neovim')
local ok, err = xpcall(function()
   vim.cmd('runtime plugin/sos.lua')
   local sos = require('sos')
   sos.setup({socket=vim.env.SOS_TEST_SOCKET,auto_start=false,timeout_ms=5000,
     workspace=vim.env.SOS_TEST_WORKSPACE})
   assert(vim.fn.exists(':SosConnect')==2 and vim.fn.exists(':SosSend')==2)
   local client = sos.client()
  local events = {}
  client.on_event = function(event) table.insert(events,event) end
   local session = sos.session()
   local panel = sos.panel()
  local function wait(predicate)
    assert(vim.wait(15000,predicate,10), 'Real-daemon timeout: ' .. vim.inspect(events))
  end
   vim.cmd('SosConnect')
  wait(function() return session.configured end)
  local function command(frame, terminal)
    local done, response, failure
    assert(session:request(frame,terminal,function(event,error) response,failure,done=event,error,true end))
    wait(function() return done end)
    assert(response,failure)
    return response
  end
  local provider = command({type='provider.create',name='Neovim smoke',base_url=vim.env.SOS_TEST_URL},'provider.listed').created_provider
  command({type='provider.select',provider=provider},'provider.selected')
  command({type='model.list'},'model.listed')
  command({type='model.select',provider=provider,model='local-model',context_limit=65536},'model.selected')
  -- A separate fresh editor must recover saved defaults on opening, before sending.
  local initialized=vim.fn.system({vim.v.progpath,'--headless','-u','NONE','-i','NONE','-l',
    'integrations/neovim/tests/initial_info.lua'})
  assert(vim.v.shell_error==0,initialized)
     vim.cmd('SosToggle')
   session:set_draft('Unsent mode-switch draft'); session:changed()
   local composer_focus=vim.api.nvim_get_current_win()
   for _,binding in ipairs({{'n','slop'},{'i','sane'}}) do
     local mapping=vim.api.nvim_buf_call(session.current.view.input,function()
       return vim.fn.maparg('<Tab>',binding[1],false,true)
     end)
     assert(mapping.buffer==1 and mapping.callback,'Composer Tab mapping is missing')
     mapping.callback()
     wait(function() return session.current.mode==binding[2] end)
     assert(not session.current.id and not session.current.busy and #session.current.records==0,
       'Composer Tab sent the draft or created a session')
     assert(session.current.draft=='Unsent mode-switch draft' and vim.api.nvim_get_current_win()==composer_focus,
       'Real-daemon mode acknowledgement changed the draft or focus')
   end
    session:set_draft('Hello\nfrom Neovim'); session:changed(); vim.cmd('SosSend')
   assert(session.current.busy and session.current.draft=='' and #session.current.records==2)
   assert(session.current.records[2]._started_at,'Activity did not start before daemon acceptance')
   session:set_draft('Queued follow-up'); session:changed(); vim.cmd('SosSend')
   assert(session.current.queued=='Queued follow-up' and session.current.draft=='')
   wait(function() return client.transport.state=='ready' and session.current.id and not session.current.busy and not session.current.pending and not session.current.queued end)
   local users=0
   for _,record in ipairs(session.current.records) do if record.role=='user' then users=users+1 end end
   assert(users==2,'Optimistic turns duplicated or lost during real-daemon reconciliation')
   local id = session.current.id
  local output = table.concat(vim.api.nvim_buf_get_lines(session.current.view.transcript,0,-1,false),'\n')
  assert(output:find('Local answer',1,true), output)
  local function metrics()
    for _,record in ipairs(session.current.records) do
      if record.role=='activity' then
        assert(record.count==0 and record.metadata.duration_ms>=0)
        assert(record.model_call.output_tokens==10,'Plain response usage was not attached to its Activity')
        local text=require('sos.tool_format').activity(record,{})
        if record.model_call.duration_ms>0 then assert(text:find('tok/s',1,true),text) end
        return
      end
    end
    error('Plain response lost its empty Activity')
  end
  metrics()
  session:set_draft('Retained session draft')
  session:new_session()
  wait(function() return session.current.id == nil and not session.switching end)
  assert(session.saved[id].draft == 'Retained session draft')
  session:attach(id)
  wait(function() return session.current.id == id and not session.switching end)
  assert(session.current.draft == 'Retained session draft' and #session.current.records > 0)
  session:latest()
  wait(function() return not session:history().loading end)
  assert(session:history().page.first and #session.current.records > 0)
  metrics()
  session:toggle_mode()
  wait(function() return session.current.mode == 'slop' end)
  -- Real editing results and shell completion must refresh open files, not drafts.
  local buffers={}
  for _,name in ipairs({'write','edit','patch','shell','dirty'}) do
    local path=vim.env.SOS_TEST_WORKSPACE..'/refresh-'..name..'.txt'
    vim.fn.writefile({'before'},path)
    local buffer=vim.fn.bufadd(path); vim.fn.bufload(buffer)
    buffers[name]=buffer
  end
  vim.api.nvim_buf_set_lines(buffers.dirty,0,-1,false,{'unsaved editor text'})
  local select=require('sos.ui').select
  require('sos.ui').select=function(rows,options,callback)
    for _,row in ipairs(rows) do
      if type(row)=='table' and row.decision=='yes' then callback(row); return end
    end
    select(rows,options,callback)
  end
  local focus=vim.api.nvim_get_current_win()
  session:set_draft('Refresh loaded files'); session:send()
  wait(function() return not session.current.busy and not session.current.pending and not session.file_refresh.scheduled end)
  require('sos.ui').select=select
  for _,name in ipairs({'write','edit','patch','shell'}) do
    assert(table.concat(vim.api.nvim_buf_get_lines(buffers[name],0,-1,false),'\n')==name..' result',
      'Real '..name..' did not refresh its open buffer')
  end
  assert(vim.bo[buffers.dirty].modified and vim.api.nvim_buf_get_lines(buffers.dirty,0,-1,false)[1]=='unsaved editor text')
  assert(vim.fn.readfile(vim.env.SOS_TEST_WORKSPACE..'/refresh-dirty.txt')[1]=='agent disk result')
  assert(vim.api.nvim_get_current_win()==focus,'Real editing refresh stole focus')
  for _,buffer in pairs(buffers) do vim.api.nvim_buf_delete(buffer,{force=true}) end
  session:set_draft('Retained session draft')
  local keeper = require('sos.transport').new({socket=vim.env.SOS_TEST_SOCKET,auto_start=false,timeout_ms=5000})
  keeper:connect()
  wait(function() return keeper.state == 'ready' end)
  client:close()
  client:reconnect()
  wait(function() return client.synchronized and not client.recovering end)
  assert(session.current.id == id and session.current.draft == 'Retained session draft')
  panel:hide()
  client:close()
  keeper:close()
end,debug.traceback)
if not ok then io.stderr:write(err .. '\n'); vim.cmd('cquit 1') end
print('Real-daemon native session smoke passed')
vim.cmd('qa!')
