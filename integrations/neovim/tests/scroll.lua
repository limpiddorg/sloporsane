-- Use a real UI loop: native page keys, wrapped rows and delayed WinScrolled events matter.
local child=vim.fn.jobstart({vim.v.progpath,'--embed','--headless','-u','NONE','-i','NONE'},{rpc=true})
assert(child>0,'Could not launch the scroll fixture')
local function exec(code,...) return vim.rpcrequest(child,'nvim_exec_lua',code,{...}) end
local function input(keys) vim.rpcrequest(child,'nvim_input',keys) end
local function wait(predicate,message) assert(vim.wait(2000,predicate,10),message) end
local ok,err=xpcall(function()
  vim.rpcrequest(child,'nvim_ui_attach',90,32,{})
  exec([[
    local runtime=...
    vim.opt.runtimepath:prepend(runtime)
    vim.o.scrolloff=7
    scroll_origin=vim.api.nvim_get_current_win()
    scroll_client={transport={state='ready',pending={}},conversation={},operations={},sent={}}
    function scroll_client:request(frame,terminal,callback)
      self.sent[#self.sent+1]={frame=frame,callback=callback}
      return tostring(#self.sent)
    end
    scroll_session=require('sos.session').new(scroll_client,{workspace='/project',progress=false})
    scroll_session.configured,scroll_session.ever_connected=true,true
    scroll_session.current.id='scroll'
    function scroll_rows(prefix,count)
      local rows={}
      for n=1,count do rows[n]=prefix..' '..n end
      return table.concat(rows,'\n')
    end
    function scroll_position()
      return require('sos.scroll').position(scroll_panel.transcript)
    end
    function scroll_same(before,label)
      local after=scroll_position()
      for _,field in ipairs({'lnum','col','topline','skipcol','leftcol'}) do
        assert(before[field]==after[field],label..' moved '..field..': '..vim.inspect({before,after}))
      end
    end
    function scroll_bottom()
      local pos=scroll_position()
      return vim.api.nvim_win_text_height(scroll_panel.transcript,{start_row=pos.topline-1,start_vcol=pos.skipcol}).all
        <=vim.api.nvim_win_get_height(scroll_panel.transcript)
    end
    function scroll_live(messages)
      scroll_session:history():live({messages=messages})
      scroll_session:changed()
    end
    function scroll_send(text)
      scroll_session.current.busy=false
      scroll_session:set_draft(text)
      scroll_session:changed()
      scroll_session:send()
    end
    function scroll_accept(id,text)
      local request=scroll_client.sent[#scroll_client.sent]
      request.callback({type='message.accepted'})
      scroll_live({{id=id,role='user',content={{type='text',text=text}}}})
    end
    scroll_session:history():install({
      {id='u1',role='user',content='Old question'},
      {id='a1',role='assistant',content=scroll_rows('Old answer',45)},
      {id='u2',role='user',content='Previous question'},
      {id='a2',role='assistant',content=scroll_rows('Previous answer',45)},
    },{})
    scroll_panel=require('sos.panel').new(scroll_session)
    scroll_panel:show()
    vim.api.nvim_set_current_win(scroll_panel.input)
    assert(scroll_bottom(),'Opening the latest history did not show the tail')
    assert(vim.wo[scroll_origin].scrolloff==7 and not vim.wo[scroll_origin].smoothscroll,
      'Chat scroll settings leaked into the file window')
    vim.api.nvim_win_call(scroll_panel.transcript,function() vim.cmd('normal! ggzt') end)
    scroll_send('New multiline\nquestion')
    assert(scroll_session.current.pending,'Send did not remain pending before acknowledgement')
    assert(scroll_position().topline==scroll_session.current.view.projection.user.line,
      'Sending from old history did not place the optimistic user turn at the top')
    assert(scroll_bottom(),'Short pending turn did not leave space below it')
    assert(vim.api.nvim_get_current_win()==scroll_panel.input,'Send stole focus')
    scroll_sent_top=scroll_position().topline
  ]],vim.env.SOS_TEST_RUNTIME or vim.fn.getcwd()..'/integrations/neovim')
  input('<PageUp>')
  wait(function() return exec('return scroll_position().topline<scroll_sent_top') end,'Composer PageUp did not scroll the transcript')
  exec([[
    scroll_reading=scroll_position()
    scroll_accept('accepted','New multiline\nquestion')
    scroll_same(scroll_reading,'Accepted echo')
    scroll_live({{id='answer',role='assistant',content={{type='text',text=scroll_rows('Reply',45)}}}})
    scroll_same(scroll_reading,'Reply while reading history')
    assert(vim.api.nvim_get_current_win()==scroll_panel.input and vim.api.nvim_get_mode().mode=='n')
  ]])
  for _=1,8 do input('<PageDown>') end
  wait(function() return exec('return scroll_bottom()') end,'Composer PageDown did not return to the bottom')
  exec([[
    scroll_live({{id='resumed',role='assistant',content={{type='text',text=scroll_rows('Followed reply',10)}}}})
    assert(scroll_bottom(),'Returning to the bottom did not resume following')
    scroll_send('Short turn')
    scroll_accept('short-turn','Short turn')
    scroll_live({{id='short-answer',role='assistant',content={{type='text',text='Short reply'}}}})
    assert(scroll_position().topline==scroll_session.current.view.projection.user.line,
      'Short reply pulled old history back into view')
    scroll_short_top=scroll_position().topline
    scroll_live({{id='short-answer-2',role='assistant',content={{type='text',text='Another short reply'}}}})
    assert(scroll_position().topline==scroll_short_top,'Short reply did not fill the space below the user')
    scroll_send('Activity turn')
    scroll_accept('activity-turn','Activity turn')
    scroll_live({{id='first-tool',role='assistant',content={{type='tool_call',toolName='read',callId='first-read',arguments={filePath='first'}}}}})
    vim.api.nvim_win_call(scroll_panel.transcript,function() vim.cmd('normal! zR') end)
    scroll_panel:sync_folds()
    scroll_activity_position=scroll_position()
    local calls={}
    for n=1,40 do calls[n]={type='tool_call',toolName='read',callId='read-'..n,arguments={filePath='file-'..n}} end
    scroll_live({{id='tools',role='assistant',content=calls}})
    scroll_same(scroll_activity_position,'Activity growth')
    assert(not scroll_bottom(),'Expanded Activity did not grow beyond the viewport')
  ]])
  exec([[
    scroll_live({{id='after-activity',role='assistant',content={{type='text',text=scroll_rows('After Activity',10)}}}})
    assert(scroll_bottom(),'Activity growth lost the intent to follow the next reply')
    scroll_session:set_draft('Retained draft')
    scroll_session:changed()
  ]])
  input('i')
  wait(function() return vim.rpcrequest(child,'nvim_get_mode').mode=='i' end,'Could not enter the composer')
  local bottom=exec('return scroll_position().topline')
  input('<PageUp>')
  wait(function() return exec('return scroll_position().topline')<bottom end,'Insert-mode PageUp did not scroll the transcript')
  assert(vim.rpcrequest(child,'nvim_get_mode').mode=='i','PageUp left Insert mode')
  exec([[
    assert(scroll_session.current.draft=='Retained draft','PageUp edited the draft')
    assert(vim.api.nvim_get_current_win()==scroll_panel.input,'PageUp stole composer focus')
    scroll_reading=scroll_position()
    scroll_live({{id='not-followed',role='assistant',content={{type='text',text=scroll_rows('Unread',20)}}}})
    scroll_same(scroll_reading,'Reply after Insert-mode PageUp')
    scroll_session:history().loading=true
    scroll_loading_position=scroll_position()
  ]])
  input('<PageDown>')
  exec([[
    scroll_same(scroll_loading_position,'PageDown during history loading')
    scroll_session:history().loading=false
  ]])
  for _=1,8 do input('<PageDown>') end
  wait(function() return exec('return scroll_bottom()') end,'Insert-mode PageDown did not return to the bottom')
  exec([[
    scroll_live({{id='insert-follow',role='assistant',content={{type='text',text=scroll_rows('Insert-mode reply',30)}}}})
    assert(scroll_bottom(),'Incoming text did not follow while editing the composer')
    assert(scroll_session.current.draft=='Retained draft','Following incoming text edited the draft')
    assert(vim.api.nvim_get_current_win()==scroll_panel.input and vim.api.nvim_get_mode().mode=='i',
      'Following incoming text changed composer focus or mode')
  ]])
  input('<Esc>')
  wait(function() return vim.rpcrequest(child,'nvim_get_mode').mode=='n' end,'Could not leave Insert mode')
  exec([[
    scroll_send('Wrapped reply')
    scroll_accept('wrapped-turn','Wrapped reply')
    scroll_live({{id='wrapped-answer',role='assistant',content={{type='text',text=string.rep('wrapped words ',200)}}}})
    vim.cmd('redraw!')
    assert(scroll_bottom(),'Automatic following missed the end of a single wrapped line: '..vim.inspect({
      position=scroll_position(),follow=scroll_session.current.view.follow,
      height=vim.api.nvim_win_text_height(scroll_panel.transcript,{start_row=scroll_position().topline-1,start_vcol=scroll_position().skipcol}),
      size=vim.api.nvim_win_get_height(scroll_panel.transcript)}))
    vim.api.nvim_set_current_win(scroll_panel.transcript)
  ]])
  local wrapped_end=exec('return scroll_position().col')
  input('2gk')
  wait(function() return exec('return scroll_position().col')<wrapped_end end,'Wrapped-line navigation did not run')
  exec([[
    scroll_reading=scroll_position()
    scroll_session.current.records[#scroll_session.current.records].content[1].text=
      scroll_session.current.records[#scroll_session.current.records].content[1].text..' more text'
    scroll_session:changed()
    scroll_same(scroll_reading,'Wrapped-line reading')
    vim.api.nvim_set_current_win(scroll_panel.input)
    scroll_live({{id='after-wrapped',role='assistant',content={{type='text',text=scroll_rows('Later',20)}}}})
    scroll_same(scroll_reading,'Background reply after wrapped-line reading')
    scroll_panel:hide()
    scroll_panel:show()
    scroll_same(scroll_reading,'Hide/reopen while reading')
    local discussion=scroll_session.current
    scroll_session.workflow:select({kind='planning',mode='sane',draft='',records={},workspace='/project'})
    scroll_session.workflow:select(discussion)
    scroll_same(scroll_reading,'Context round trip')
    vim.api.nvim_win_call(scroll_panel.transcript,function() vim.cmd('normal! G$') end)
    scroll_panel:render()
    vim.api.nvim_win_set_width(scroll_panel.transcript,30)
    vim.api.nvim_win_set_height(scroll_panel.input,7)
    scroll_panel:render()
    assert(scroll_bottom(),'Resizing lost the followed tail')
    scroll_session:history():install({{id='older-user',role='user',content='Older question'},
      {id='older-answer',role='assistant',content=scroll_rows('Older',45)}},{hasNewer=true})
    scroll_session:changed()
    scroll_reading=scroll_position()
    scroll_panel:render()
    scroll_same(scroll_reading,'Historical page refresh')
    scroll_session:history():install({{id='latest-user',role='user',content='Latest question'},
      {id='latest-answer',role='assistant',content='Latest answer'}},{})
    scroll_session:changed()
    assert(scroll_position().topline==scroll_session.current.view.projection.user.line,
      'Returning to latest history did not position the last turn')
    scroll_panel:hide()
    scroll_session.current={mode='sane',draft='',records={},workspace='/project'}
    scroll_session:changed()
    scroll_panel:show()
    vim.api.nvim_set_current_win(scroll_panel.input)
    scroll_send('First question')
    assert(scroll_session.current.view.follow and scroll_bottom(),'First send without an existing history lost following')
    scroll_accept('first-question','First question')
    scroll_live({{id='first-reply',role='assistant',content={{type='text',text=scroll_rows('First reply',50)}}}})
    assert(scroll_bottom(),'First reply did not follow')
    scroll_session.client.transport.dispatching=true
    scroll_live({{id='batched',role='assistant',content={{type='text',text=scroll_rows('Batched reply',30)}}}})
    scroll_session.client.transport.dispatching=false
    assert(scroll_panel.render_pending,'Network render was not deferred')
    require('sos.scroll').page(scroll_panel.transcript,scroll_session.current.view,'up')
    scroll_reading=scroll_position()
  ]])
  wait(function() return exec('return not scroll_panel.render_pending') end,'Queued render did not complete')
  exec([[
    scroll_same(scroll_reading,'Scrolling before a queued render')
    vim.o.mouse='a'
    vim.api.nvim_win_call(scroll_panel.transcript,function() vim.cmd('normal! G$') end)
    scroll_panel:render()
    scroll_mouse_top=scroll_position().topline
  ]])
  local position=exec('return vim.api.nvim_win_get_position(scroll_panel.transcript)')
  for _=1,3 do vim.rpcrequest(child,'nvim_input_mouse','wheel','up','',0,position[1]+3,position[2]+3) end
  wait(function() return exec('return scroll_position().topline<scroll_mouse_top') end,'Mouse wheel did not scroll the background transcript')
  exec([[
    assert(vim.api.nvim_get_current_win()==scroll_panel.input,'Mouse wheel stole composer focus')
    scroll_reading=scroll_position()
    scroll_live({{id='after-wheel',role='assistant',content={{type='text',text=scroll_rows('After wheel',30)}}}})
    scroll_same(scroll_reading,'Reply after background mouse scrolling')
    local records={}
    for n=1,10 do
      records[#records+1]={id='question-'..n,role='user',content='Old question '..n}
      records[#records+1]={id='reply-'..n,role='assistant',content=scroll_rows('Old reply '..n,3)}
    end
    scroll_session:history():install(records,{})
    scroll_session:changed()
    scroll_long_question=scroll_rows('Long question',45)
    scroll_send(scroll_long_question)
    require('sos.scroll').page(scroll_panel.transcript,scroll_session.current.view,'up')
    local before=scroll_position()
    local user_line=scroll_session.current.view.projection.user.line
    assert(before.topline>=user_line,'Fixture did not scroll within the optimistic turn')
    scroll_accept('long-question',scroll_long_question)
    local delta=scroll_session.current.view.projection.user.line-user_line
    assert(delta<0,'Acceptance did not exercise history eviction')
    before.topline,before.lnum=before.topline+delta,before.lnum+delta
    scroll_same(before,'Echo while evicting the oldest history record')
    scroll_panel:hide()
  ]])
end,debug.traceback)
vim.fn.jobstop(child)
assert(ok,err)
print('TUI-aligned turn anchoring, reply following, Activity stability and native composer paging passed')
