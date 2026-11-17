-- Attached UI checks the review chat and origin-sensitive native inspection path.
local child=vim.fn.jobstart({vim.v.progpath,'--embed','--headless','-u','NONE','-i','NONE'},{rpc=true})
assert(child>0)
local function exec(code,...) return vim.rpcrequest(child,'nvim_exec_lua',code,{...}) end
local function input(keys) vim.rpcrequest(child,'nvim_input',keys) end
local function wait(code)
  if vim.wait(2000,function() return exec(code) end,10) then return end
  error(code..'\n'..exec([=[local rows={}
    for row=1,vim.o.lines do
      local text=''
      for col=1,vim.o.columns do text=text..vim.fn.screenstring(row,col) end
      rows[#rows+1]=text
    end
    return table.concat(rows,'\n')]=]))
end
local function command(name) input(':'..name..'<CR>') end
local function choose(identity)
  exec([=[
    for n,row in ipairs(w.selector.rows) do
      if row.key==... then api.nvim_win_set_cursor(w.selector.window,{n,0}); return end
    end
    error('Missing step')
  ]=],identity)
  input('<CR>')
  wait('return not w.selector.window and s.inspector and s.inspector.visible')
end
local ok,err=xpcall(function()
  vim.rpcrequest(child,'nvim_ui_attach',120,42,{})
  exec([=[
    vim.opt.runtimepath:prepend(...)
    api=vim.api
    editor=api.nvim_get_current_win()
    user=api.nvim_get_current_buf()
    api.nvim_buf_set_lines(user,0,-1,false,{'Unsaved editor text','Keep this too'})
    vim.wo[editor].number=false; vim.wo[editor].wrap=true
    vim.wo[editor].winbar='My editor'; vim.wo[editor].statusline='My status'
    client={transport={state='ready',pending={}},conversation={},operations={},sent={}}
    function client:request(frame,terminal,callback)
      self.sent[#self.sent+1]={frame=frame,terminal=terminal,callback=callback}
      return tostring(#self.sent)
    end
    function client:reply(event)
      local request=self.sent[tonumber(event.requestId)]
      if request and request.callback then request.callback(event) end
      s:event(event)
    end
    s=require('sos.session').new(client,{workspace='/project'})
    s.configured=true
    s.current.id='discussion'; s.current.kind='discussion'; s.current.draft='Discussion draft'
    s.current.records={{id='discussion-turn',role='assistant',content='Preserved Discussion'}}
    discussion=s.current; s:changed()
    w=s.workflow
    panel=require('sos.panel').new(s); panel:show()
    local plugin=require('sos')
    plugin.session=function() return s end
    plugin.panel=function() return panel end
    vim.cmd('runtime plugin/sos.lua')
    require('sos.ui').select=function(rows,options,callback) popup={rows=rows,options=options,callback=callback} end
    w.branch_id='branch'; w.attached_id='branch'; w.discussion_id='discussion'
    w.info={workspace='/project',implementation_view=true,implementation_plan_ready=true,implementation_plan={content='Final plan'}}
    s.info=w.info
    w:show_plan(); s:implement()
    assert(not w.state and popup.options.prompt=='Implementation pace')
    popup.callback(popup.rows[1]); popup=nil
    assert(client.sent[#client.sent].frame.type=='implementation.start' and s.current.kind=='plan')
    diff={path='feature.lua',before='old\nkeep\nold again\n',after='new\nkeep\nnew again\n',
      patch='@@ -1 +1 @@\n-old\n+new\n@@ -3 +3 @@\n-old again\n+new again',hunk_explanations={'First explanation','Second explanation'}}
    function part() return {complete=false,reviewed=false,explanation='',activity={},diffs={}} end
    state={status='running',preparing=true,pace='everything',revision=1,work_step=0,work_substep=0,review_step=0,
      plan={steps={{title='Build the feature',substeps={{title='Wire it'},{title='Explain only'}}},
        {title='Integrate the result',substeps={{title='Connect it'}}}}},
      steps={{identity='first',generation=1,whole=part(),substeps={part(),part()},validation=part()},
        {identity='second',generation=1,whole=part(),substeps={part()},validation=part()}}}
    function update() client:reply({type='implementation.updated',sessionId=w.branch_id,implementation=vim.deepcopy(state),info=w.info}) end
    function text() return table.concat(api.nvim_buf_get_lines(s.current.view.transcript,0,-1,false),'\n') end
    function input_text() return table.concat(api.nvim_buf_get_lines(s.current.view.input,0,-1,false),'\n') end
    update()
    assert(s.current.kind=='review' and s.current.step==0 and #s.current.records==0)
    assert(panel.input and not vim.bo[s.current.view.input].modifiable and not w.selector and not s.inspector)
    assert(input_text():find('Building step 1',1,true) and input_text():find(':SosSteps to select a step to review',1,true))
    assert(not text():find('Final plan',1,true) and not text():find('Preserved Discussion',1,true))
    assert(discussion.draft=='Discussion draft' and discussion.records[1].content=='Preserved Discussion')
    assert(api.nvim_get_current_win()==editor and api.nvim_win_get_buf(editor)==user)
    local count=#client.sent
    s:cancel()
    assert(popup and popup.options.prompt=='Interrupt implementation?','The building placeholder routed Cancel to review')
    popup.callback(popup.rows[1]); popup=nil
    assert(#client.sent==count,'Declining construction cancellation sent a request')
    api.nvim_set_current_win(panel.input)
  ]=],vim.env.SOS_TEST_RUNTIME or vim.fn.getcwd()..'/integrations/neovim')
  input('iUnexpected typing<Esc>')
  wait("return api.nvim_get_mode().mode=='n'")
  assert(exec("return s.current.draft=='' and input_text():find('Building step 1',1,true) and not vim.bo[s.current.view.input].modifiable"),
    'Locked construction status became a draft')
  exec('api.nvim_set_current_win(panel.transcript)')
  command('SosSteps')
  wait('return w.selector and w.selector:visible()')
  exec([=[
    assert(w.selector.origin.window==panel.transcript and w.selector.origin.from_chat)
    assert(api.nvim_win_get_config(w.selector.window).relative=='editor','Step choices are not a popup')
    assert(not s.inspector,'Merely opening steps opened code')
    assert(#w.selector.rows==2 and w.selector.rows[1].key=='first' and w.selector.rows[2].key=='second',
      'The selector contains non-step choices')
    function preview_text() return table.concat(api.nvim_buf_get_lines(w.selector.popup.preview_buffer,0,-1,false),'\n') end
    assert(preview_text():find('1. Wire it',1,true) and preview_text():find('2. Explain only',1,true))
    local left=api.nvim_win_get_config(w.selector.popup.preview_window)
    local right=api.nvim_win_get_config(w.selector.window)
    assert(left.col+left.width+2<right.col and left.row==right.row and left.height==right.height)
    local cursor=api.nvim_win_get_cursor(w.selector.window)[1]
    state.preparing=false; state.steps[1].substeps[1].diffs={diff}; update()
    assert(api.nvim_get_current_win()==w.selector.window and api.nvim_win_get_cursor(w.selector.window)[1]==cursor)
  ]=])
  input('j')
  wait("return preview_text():find('1. Connect it',1,true) and not preview_text():find('Wire it',1,true)")
  exec([=[
    assert(not s.inspector and s.current.step==0 and w.selected_identity=='first','Preview changed the active review')
    state.steps[2].substeps[1].explanation='Preview the integration without opening it'; update()
    assert(preview_text():find('Preview the integration without opening it',1,true))
  ]=])
  vim.rpcrequest(child,'nvim_ui_try_resize',60,20)
  exec([=[
    for _,win in ipairs({w.selector.window,w.selector.popup.preview_window}) do
      local cfg=api.nvim_win_get_config(win)
      assert(cfg.col>=0 and cfg.col+cfg.width+2<=vim.o.columns and cfg.row+cfg.height+2<=vim.o.lines)
    end
  ]=])
  vim.rpcrequest(child,'nvim_ui_try_resize',120,42)
  exec([=[
    state.steps[2].substeps[1].explanation=string.rep('Long preview explanation\n',60); update()
    preview_window=w.selector.popup.preview_window
    preview_buffer=w.selector.popup.preview_buffer
  ]=])
  input('<C-d>')
  wait('return api.nvim_win_get_cursor(preview_window)[1]>1')
  exec([=[
    local position=api.nvim_win_get_cursor(preview_window)[1]
    update()
    assert(api.nvim_win_get_cursor(preview_window)[1]==position and api.nvim_win_get_cursor(w.selector.window)[1]==2)
    assert(api.nvim_get_current_win()==w.selector.window and not s.inspector,'Preview paging opened inspection or moved list focus')
  ]=])
  choose('first')
  exec([=[
    assert(not api.nvim_win_is_valid(preview_window) and not api.nvim_buf_is_valid(preview_buffer),'Selection leaked the preview frame')
    assert(s.inspector.code~=panel.transcript and s.inspector.code~=panel.input and s.inspector.code~=editor,
      'Selection from chat replaced a chat or project window')
    assert(api.nvim_win_get_buf(editor)==user and api.nvim_buf_get_lines(user,0,1,false)[1]=='Unsaved editor text')
    assert(s.inspector.explanation and not vim.bo[s.inspector.code_buffer].modifiable)
    assert(vim.wo[s.inspector.explanation].winbar=='Explanation')
    assert(api.nvim_buf_get_lines(s.inspector.header_buffer,0,1,false)[1]=='Step 1/2 | Substep 1/2 | Diff 1/2 | Wire it')
    assert(api.nvim_buf_get_lines(s.inspector.header_buffer,1,2,false)[1]=='feature.lua')
    assert(table.concat(api.nvim_buf_get_lines(s.inspector.explanation_buffer,0,-1,false),'\n')=='First explanation')
    vim.cmd('redraw')
    local explanation_pos=api.nvim_win_get_position(s.inspector.explanation)
    local header_pos=api.nvim_win_get_position(s.inspector.header)
    local code_pos=api.nvim_win_get_position(s.inspector.code)
    assert(explanation_pos[1]<header_pos[1] and header_pos[1]+2<code_pos[1])
    local function screen(row,col,length)
      local text=''
      for x=col,col+length-1 do text=text..vim.fn.screenstring(row,x) end
      return text
    end
    assert(screen(explanation_pos[1]+1,explanation_pos[2]+1,11)=='Explanation')
    assert(screen(header_pos[1]+2,header_pos[2]+1,11)=='feature.lua','The filename is not on its own header line')
    chat_code=s.inspector.code
    assert(text()=='','Construction evidence leaked into review history')
    state.steps[1].whole.complete=true; state.steps[1].whole.diffs={diff}
    state.steps[1].substeps[1].complete=true; state.steps[1].substeps[2].complete=true
    state.work_step=1; state.work_substep=0
    update()
    assert(not w:readonly(s.current) and vim.bo[s.current.view.input].modifiable,'Eligible selected review did not unlock')
    assert(s.current.substep==0 and s.current.question_context.path=='feature.lua')
    client:reply({type='harness.message_arrived',sessionId='branch',messages={{id='construction',role='assistant',content='Do not put this in review'}}})
    assert(text()=='','Construction messages entered the independent review')
    client:reply({type='btw.started',sessionId='branch',requestId='review-transfer',messageCount=1,branch={review=true,review_step=0,status='completed'}})
    client:reply({type='btw.chunk',sessionId='branch',requestId='review-transfer',messageIndex=0,partIndex=0,last=true,
      data=vim.json.encode({id='review-answer',role='assistant',content='Only the review answer'})})
    client:reply({type='btw.finished',sessionId='branch',requestId='review-transfer',messageCount=1})
    assert(text():find('Only the review answer',1,true) and api.nvim_get_current_win()==chat_code)
    s.current.draft='Keep my review draft'; s:changed()
    requests=#client.sent
  ]=])
  command('SosNextDiff'); wait('return s.inspector.model.change==2')
  assert(exec([=[return api.nvim_buf_get_lines(s.inspector.header_buffer,0,1,false)[1]:find('Substep 1/2 | Diff 2/2',1,true)
    and table.concat(api.nvim_buf_get_lines(s.inspector.explanation_buffer,0,-1,false),'\n'):find('Second explanation',1,true)]=]))
  command('SosPreviousDiff'); wait('return s.inspector.model.change==1')
  command('SosNextSubstep'); wait('return s.inspector.model.part==1 and s.inspector.model.change==0')
  command('SosPreviousSubstep'); wait('return s.inspector.model.part==0 and s.current.draft=="Keep my review draft"')
  command('SosExplanations'); wait('return s.inspector.collapsed and not s.inspector.explanation')
  command('SosExplanations'); wait('return not s.inspector.collapsed and api.nvim_win_is_valid(s.inspector.explanation)')
  exec([=[
    assert(#client.sent==requests,'Inspection navigation sent an execution or review request')
    api.nvim_win_set_cursor(s.inspector.code,{2,0})
    state.steps[2].substeps[1].explanation='Background progress'; update()
    assert(api.nvim_get_current_win()==chat_code and api.nvim_win_get_cursor(chat_code)[1]==2)
    api.nvim_set_current_win(panel.input)
  ]=])
  command('SosSteps'); wait('return w.selector and w.selector:visible()')
  exec('assert(w.selector.origin.window==panel.input and w.selector.origin.from_chat)')
  choose('first')
  assert(exec('return s.inspector.code==chat_code and s.current.draft=="Keep my review draft"'),'Selection from input duplicated code or lost the draft')
  exec('s.inspector:hide(); api.nvim_set_current_win(editor)')
  command('SosSteps'); wait('return w.selector and w.selector:visible()')
  exec('cancel_preview=w.selector.popup.preview_window; cancel_buffer=w.selector.popup.preview_buffer')
  input('<Esc>')
  wait('return not w.selector.window and api.nvim_get_current_win()==editor')
  assert(exec('return api.nvim_win_get_buf(editor)==user and not s.inspector.visible'),'Cancelling step choices replaced the editor')
  assert(exec('return not api.nvim_win_is_valid(cancel_preview) and not api.nvim_buf_is_valid(cancel_buffer)'),
    'Cancelling left the preview window or buffer behind')
  command('SosSteps'); wait('return w.selector and w.selector:visible()')
  exec('assert(w.selector.origin.window==editor and not w.selector.origin.from_chat)')
  choose('first')
  exec([=[
    assert(s.inspector.code==editor and api.nvim_get_current_win()==editor,'Editor-origin selection did not reuse its window')
    assert(s.inspector.borrowed.buffer==user and vim.bo[user].modified and vim.bo[user].modifiable)
    assert(api.nvim_buf_is_loaded(user) and api.nvim_buf_get_lines(user,0,1,false)[1]=='Unsaved editor text','Replacement discarded user edits')
    assert(s.inspector.model.change==1 and s.current.question_context.path=='feature.lua')
    assert(api.nvim_buf_get_lines(s.inspector.header_buffer,0,1,false)[1]:find('Substep 1/2 | Diff 1/2',1,true))
  ]=])
  input('q')
  wait('return not s.inspector.visible and api.nvim_win_get_buf(editor)==user')
  exec([=[
    assert(vim.wo[editor].winbar=='My editor' and vim.wo[editor].statusline=='My status' and not vim.wo[editor].number and vim.wo[editor].wrap,
      'Closing borrowed inspection did not restore window options')
    assert(vim.bo[user].modified and api.nvim_buf_get_lines(user,0,1,false)[1]=='Unsaved editor text')
  ]=])
  command('SosSteps'); wait('return w.selector and w.selector:visible()')
  exec([=[
    replacement=api.nvim_create_buf(true,false)
    api.nvim_buf_set_lines(replacement,0,-1,false,{'New editor contents'})
    api.nvim_win_set_buf(editor,replacement)
    for n,row in ipairs(w.selector.rows) do if row.key=='first' then api.nvim_win_set_cursor(w.selector.window,{n,0}); break end end
  ]=])
  input('<CR>')
  wait('return not w.selector.window and not s.inspector.visible')
  exec([=[
    assert(api.nvim_win_get_buf(editor)==replacement,'A stale popup replaced a newly changed editor buffer')
    api.nvim_win_set_buf(editor,user)
    panel:hide()
    assert(not w.selector and not panel.input)
    panel:show()
    assert(s.current.kind=='review' and panel.input and s.current.draft=='Keep my review draft' and text():find('Only the review answer',1,true))
    w:select_step('second')
    assert(not vim.bo[s.current.view.input].modifiable and input_text():find('Building step 2',1,true))
    assert(not text():find('Only the review answer',1,true),'Another step inherited the first review history')
    state.status='interrupted'; update()
    assert(input_text():find(':SosContinue',1,true) and not s.current.view.projection.running_activity)
    panel:hide(); panel:show()
    assert(s.current.step==1 and input_text():find('Interrupted',1,true))
    for _,request in ipairs(client.sent) do assert(request.frame.type~='implementation.resume','Browsing resumed construction') end
  ]=])
  vim.rpcrequest(child,'nvim_ui_try_resize',60,14)
  exec('api.nvim_win_set_width(panel.transcript,30)')
  wait([=[vim.cmd('redraw')
    local found=false
    for row=1,vim.o.lines do
      local text=''
      for col=1,vim.o.columns do text=text..vim.fn.screenstring(row,col) end
      if text:find('Interrupted',1,true) then found=true end
    end
    return found and api.nvim_win_get_height(panel.input)>=1 and api.nvim_win_get_height(panel.transcript)>=1
  ]=])
  exec('panel:hide()')
end,debug.traceback)
pcall(vim.rpcrequest,child,'nvim_command','qa!')
pcall(vim.fn.jobwait,{child},2000)
assert(ok,err)
print('Build-to-review chat, locked status, popup origins, native inspection and explanation commands passed')
