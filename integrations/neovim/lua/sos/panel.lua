local M = {}
local api = vim.api
local Render = require('sos.render')
local Highlights = require('sos.highlights')
local Scroll = require('sos.scroll')
local Layout = require('sos.layout')
local root=vim.fn.fnamemodify(debug.getinfo(1,'S').source:sub(2),':h:h:h')
local ok,lines=pcall(vim.fn.readfile,root..'/VERSION','',1)
local version=ok and lines[1] or 'dev'
if not version or not version:match('^v?%d+%.%d+%.%d+[%w.%-]*$') then version='dev' end
if version~='dev' then version='v'..version:gsub('^v','') end
local function valid(win) return win and api.nvim_win_is_valid(win) end

function M.new(session)
  Highlights.setup()
  local self = setmetatable({session=session}, {__index=M})
  session.panel=self
  session.on_change = function()
    if session.client.transport.dispatching then self:queue_render() else self:render() end
  end
  local group=api.nvim_create_augroup('SosComposer'..tostring(vim.uv.hrtime()),{clear=true})
  api.nvim_create_autocmd('WinResized',{group=group,callback=function() self:queue_render() end})
  api.nvim_create_autocmd({'WinClosed','BufDelete','BufWipeout'},{group=group,callback=function(event)
    local window=event.event=='WinClosed'
    local closed=window and tonumber(event.match) or event.buf
    local function matches(win,buf) return closed==(window and win or (not window and buf)) end
    local view=self.displayed and self.displayed.view or {}
    local inspector=session.inspector
    local selector=session.workflow.selector
    local popup=selector and selector.popup
    local surface=self.build_surface
    local footer=inspector and inspector.header_surface
    local function surface_matches(item)
      return item and not item.closed and (matches(item.window,item.buffer) or matches(item.float,item.display))
    end
    local function owned()
      return matches(self.input,view.input) or matches(self.transcript,view.transcript)
        or surface_matches(surface) and surface==self.build_surface
        or inspector and inspector.visible and (matches(inspector.code,inspector.code_buffer)
          or matches(inspector.explanation,inspector.explanation_buffer)
          or surface_matches(footer) and footer==inspector.header_surface)
        or popup and not popup.closed and (matches(popup.window,popup.buffer) or matches(popup.preview_window,popup.preview_buffer))
    end
    if not (self.input or self.transcript) or not owned() then return end
    local input,transcript=self.input,self.transcript
    -- Let Neovim finish teardown before closing the rest of the SOS layout.
    vim.schedule(function()
      if self.input==input and self.transcript==transcript then self:hide() end
    end)
  end})
  return self
end

function M:queue_render()
  if self.render_pending or not valid(self.transcript) then return end
  self.render_pending=true
  vim.schedule(function() if self.render_pending then self:render() end end)
end

function M:view(current)
  if current.view then return current.view end
  local transcript, input = api.nvim_create_buf(false, true), api.nvim_create_buf(false, true)
  vim.bo[transcript].buftype = 'nofile'
  vim.bo[transcript].filetype = 'sos'
  vim.bo[transcript].modifiable = false
  vim.bo[transcript].readonly = true
  vim.bo[input].buftype = 'nofile'
  vim.bo[input].filetype = 'sos_input'
  vim.bo[input].bufhidden = 'hide'
  vim.bo[input].textwidth = 0
  vim.bo[input].omnifunc = 'v:lua.require"sos.actions".complete'
  api.nvim_buf_set_lines(input, 0, -1, false, vim.split(current.draft, '\n', {plain=true}))
  api.nvim_buf_attach(input, false, {on_lines=function()
    if self.updating then return end
    current.draft = table.concat(api.nvim_buf_get_lines(input,0,-1,false), '\n')
    if self.session.current == current then self.session:set_draft(current.draft) end
  end})
  current.view = {transcript=transcript,input=input}
  local view = current.view
  view.open = {}
  vim.keymap.set({'n','i'},'<Tab>',function() self.session:toggle_mode() end,
    {buffer=input,silent=true,desc='Toggle SOS Sane/Slop mode'})
  for key,direction in pairs({['<PageUp>']='up',['<PageDown>']='down'}) do
    vim.keymap.set({'n','i'},key,function()
      if valid(self.transcript) and self.displayed==current and not (current.history and current.history.loading) then
        Scroll.page(self.transcript,view,direction)
      end
    end,{buffer=input,silent=true,desc='Scroll SOS transcript '..direction})
  end
  vim.keymap.set('n','i',function()
    if valid(self.input) then api.nvim_set_current_win(self.input)
    elseif current.kind=='step' or current.kind=='validation' then self.session:review_question() end
  end,{buffer=transcript,silent=true,desc='Focus SOS composer or eligible review'})
  vim.keymap.set('n','q',function() self:hide() end,
    {buffer=transcript,silent=true,desc='Hide SOS chat'})
  -- Preserve native fold commands, then observe their resulting state.
  for _, command in ipairs({'zo','zO','zc','zC','za','zA','zr','zR','zm','zM','zx','zv'}) do
    vim.keymap.set('n',command,function()
      vim.cmd('normal! '..(vim.v.count > 0 and tostring(vim.v.count) or '')..command)
      self:sync_folds()
    end,{buffer=transcript,silent=true})
  end
  vim.keymap.set('n','<CR>',function()
    local control=(view.projection.controls or {})[vim.api.nvim_win_get_cursor(0)[1]]
    if control then
      if control=='close_implementation' then self.session.workflow:discussion()
      else self.session[control](self.session) end
    elseif vim.fn.foldlevel('.') > 0 then
      vim.cmd('normal! za')
      self:sync_folds()
    end
  end,{buffer=transcript,silent=true,desc='Use SOS action or toggle fold'})
  vim.keymap.set('n','gx',function()
    local cursor=api.nvim_win_get_cursor(0)
    for _,link in ipairs((view.projection.links or {})[cursor[1]] or {}) do
      if cursor[2]>=link.first and cursor[2]<link.last and link.url:match('^https?://') then
        vim.ui.open(link.url); return
      end
    end
  end,{buffer=transcript,silent=true,desc='Open SOS Markdown link'})
  api.nvim_create_autocmd({'CursorMoved','WinScrolled','SafeState'}, {
    callback=function()
      if not self.rendering and self.displayed==current and valid(self.transcript) then
        Scroll.observe(self.transcript,view)
        if api.nvim_get_current_buf() == transcript then self:sync_folds() end
      end
    end,
  })
  return current.view
end

function M:capture_folds(view)
  if not view.projection or not valid(self.transcript) then return end
  api.nvim_win_call(self.transcript,function()
    for _, span in ipairs(view.projection.spans) do
      local closed = vim.fn.foldclosed(span.start)
      if closed == -1 then view.open[span.key] = true
      elseif closed == span.start then view.open[span.key] = false end
    end
  end)
end

function M:sync_folds()
  if self.syncing or self.rendering or not valid(self.transcript) then return end
  local current = self.session.current
  if self.displayed ~= current then return end
  local view = self:view(current)
  self:capture_folds(view)
  local loads = {}
  view.observed = view.observed or {}
  api.nvim_win_call(self.transcript,function()
    for _, span in ipairs((view.projection or {}).spans or {}) do
      local visible = vim.fn.foldclosed(span.start) == -1
      if visible and view.open[span.key] and span.group and not view.observed[span.key] then
        table.insert(loads,{group=span.group,index=span.index})
      end
      view.observed[span.key] = visible
    end
  end)
  self.syncing = true
  for _, selection in ipairs(loads) do self.session:history():load(selection.group,selection.index) end
  self.syncing = false
end

function M:save_view()
  if not self.displayed then return end
  local view = self:view(self.displayed)
  self:capture_folds(view)
  if valid(self.transcript) then
    view.position=Scroll.observe(self.transcript,view)
  end
  if valid(self.input) then view.cursor = api.nvim_win_get_cursor(self.input) end
end

function M:hide()
  self.render_pending=false
  self:save_view()
  require('sos.progress').close(self.session)
  local view=self.displayed and self.displayed.view
  local input,transcript=self.input,self.transcript
  local excluded={}
  for _,win in pairs({input=input,transcript=transcript,navigation=(self.session.workflow.selector or {}).window}) do excluded[win]=true end
  local sizes=Layout.heights(excluded)
  self.input, self.transcript = nil, nil
  self.session.workflow:close_steps()
  if self.session.inspector and self.session.inspector.visible then self.session.inspector:hide() end
  if view and valid(input) and api.nvim_win_get_buf(input)==view.input then api.nvim_win_close(input, true) end
  if view and valid(transcript) and api.nvim_win_get_buf(transcript)==view.transcript then api.nvim_win_close(transcript, true) end
  Layout.restore_heights(sizes)
end

function M:show()
  if valid(self.transcript) then return end
  local sizes=Layout.heights()
  local origin = api.nvim_get_current_win()
  self.origin = origin
  self.session.panel_origin=origin
  local view = self:view(self.session.current)
  vim.cmd('botright vsplit')
  self.transcript = api.nvim_get_current_win()
  api.nvim_win_set_buf(self.transcript, view.transcript)
  vim.cmd('belowright split')
  self.input = api.nvim_get_current_win()
  api.nvim_win_set_buf(self.input, view.input)
  vim.wo[self.input].wrap = true
  vim.wo[self.transcript].wrap = true
  vim.wo[self.transcript].breakindent = true
  vim.wo[self.transcript].scrolloff = 0
  vim.wo[self.transcript].smoothscroll = true
  for _,window in ipairs({self.input,self.transcript}) do
    vim.wo[window].winfixwidth=false
    vim.wo[window].winfixheight=false
    vim.wo[window].number=false
    vim.wo[window].relativenumber=false
    vim.wo[window].colorcolumn=''
    vim.wo[window].cursorline=false
  end
  vim.wo[self.input].signcolumn='no'
  vim.wo[self.input].foldcolumn='0'
  vim.wo[self.input].foldenable=false
  vim.wo[self.input].statuscolumn=''
  vim.wo[self.input].statusline=' '
  vim.wo[self.transcript].statusline=' '
  vim.wo[self.transcript].signcolumn='no'
  vim.wo[self.transcript].statuscolumn=''
  vim.wo[self.input].winhighlight = 'NormalNC:Normal,StatusLine:Normal,StatusLineNC:Normal,WinBar:Normal,WinBarNC:Normal'
  vim.wo[self.transcript].winhighlight = 'NormalNC:Normal,Folded:Normal,FoldColumn:Normal,StatusLine:Normal,StatusLineNC:Normal,WinBar:Normal,WinBarNC:Normal'
  vim.wo[self.transcript].foldminlines = 0
  vim.wo[self.transcript].foldcolumn = '0'
  vim.wo[self.transcript].foldtext = "v:lua.require'sos.render'.foldtext()"
  -- Initial sizing only; rendering and context switches preserve native resizing.
  api.nvim_win_set_height(self.input,5)
  self.displayed = nil
  self:render()
  Layout.restore_heights(sizes)
  if view.cursor then pcall(api.nvim_win_set_cursor,self.input,view.cursor) end
  if valid(origin) then api.nvim_set_current_win(origin) end
end

function M:toggle()
  if valid(self.transcript) then self:hide(); return end
  self:show()
  if valid(self.input) then api.nvim_set_current_win(self.input)
  elseif valid(self.transcript) then api.nvim_set_current_win(self.transcript) end
  self.session.workflow:steps(true)
end

function M:sync_composer(current)
  local writable=not self.session.workflow:readonly(current)
  local visible=writable or current.kind=='review'
  local displayed=self.displayed or current
  if valid(self.input) and displayed.view and api.nvim_win_get_buf(self.input)~=displayed.view.input then self.input=nil end
  local excluded={[self.transcript]=true}
  if self.input then excluded[self.input]=true end
  local sizes=Layout.heights(excluded)
  if not visible and valid(self.input) then
    local window=self.input
    self.input_height=api.nvim_win_get_height(window)
    self.input=nil
    local focused=api.nvim_get_current_win()==window
    if focused then vim.cmd('stopinsert') end
    api.nvim_win_close(window,true)
    if focused then api.nvim_set_current_win(self.transcript) end
  elseif visible and not valid(self.input) then
    local focus=api.nvim_get_current_win()
    api.nvim_win_call(self.transcript,function()
      vim.cmd('belowright '..(self.input_height or 5)..'split')
      self.input=api.nvim_get_current_win()
      api.nvim_win_set_buf(self.input,self:view(current).input)
    end)
    for key,value in pairs({wrap=true,number=false,relativenumber=false,signcolumn='no',foldcolumn='0',
      foldenable=false,statuscolumn='',statusline=' ',colorcolumn=''}) do vim.wo[self.input][key]=value end
    vim.wo[self.input].winhighlight='NormalNC:Normal,StatusLine:Normal,StatusLineNC:Normal,WinBar:Normal,WinBarNC:Normal'
    if valid(focus) then api.nvim_set_current_win(focus) end
  end
  Layout.restore_heights(sizes)
end

function M:render_input(current,view)
  local status=self.session.workflow:review_status(current)
  local content=status or current.draft
  local previous=table.concat(api.nvim_buf_get_lines(view.input,0,-1,false),'\n')
  if previous~=content then
    self.updating=true
    vim.bo[view.input].readonly=false
    vim.bo[view.input].modifiable=true
    api.nvim_buf_set_lines(view.input,0,-1,false,vim.split(content,'\n',{plain=true}))
    self.updating=false
  end
  local readonly=self.session.workflow:readonly(current)
  if readonly and valid(self.input) and api.nvim_get_current_win()==self.input then vim.cmd('stopinsert') end
  vim.bo[view.input].modifiable=not readonly
  vim.bo[view.input].readonly=readonly
  local namespace=api.nvim_create_namespace('sos-input-status')
  api.nvim_buf_clear_namespace(view.input,namespace,0,-1)
  if status then
    api.nvim_buf_set_extmark(view.input,namespace,0,0,{line_hl_group='SosActivity'})
    api.nvim_buf_set_extmark(view.input,namespace,1,0,{line_hl_group='SosMuted'})
  end
end

function M:update_input()
  if not valid(self.input) then return end
  local current=self.session.current
  if not current.view or api.nvim_win_get_buf(self.input)~=current.view.input then return end
  local info=self.session.info or {}
  local function clean(value) return tostring(value):gsub('[\r\n]',' ') end
  local function count(value)
    if type(value)~='number' or value~=value or value<0 or value==math.huge then return nil end
    return math.floor(value)
  end
  local function number(value)
    if not value then return '—' end
    return string.format('%.0f',value):reverse():gsub('(%d%d%d)','%1 '):reverse():gsub('^ ','')
  end
  local used,total=count(info.last_turn_used_context),count(info.total_context)
  local fields={
    model=type(info.model)=='string' and clean(info.model) or '—',
    reasoning=' | '..(info.reasoning=='none' and 'No reasoning' or type(info.reasoning)=='string' and clean(info.reasoning) or '—'),
    context=used==nil and total==nil and '' or number(used)..' / '..number(total),
    percent=used==nil and total==nil and '-' or (used==nil or not total or total==0) and '[—]' or '['..math.floor(100*used/total+.5)..'%]',
  }
  local width=api.nvim_win_get_width(self.input)
  local sane=current.kind=='review' or current.kind=='revision' or current.mode~='slop'
  local mode=sane and 'Sane' or 'Slop'
  local fit=require('sos.tool_format').fit
  -- AgentInfoBar.BUTTON_WIDTH is 15; ModeButton centers its label in that row.
  local badge_width=math.min(15,math.max(1,width-1))
  mode=fit(mode,badge_width)
  local padding=math.floor((badge_width-vim.fn.strdisplaywidth(mode))/2)
  local badge=string.rep(' ',padding)..mode..string.rep(' ',badge_width-padding-vim.fn.strdisplaywidth(mode))
  local function sizes()
    local left=fields.model..fields.reasoning
    local center=fields.context..(fields.context~='' and fields.percent~='' and ' ' or '')..fields.percent
    local required=vim.fn.strdisplaywidth(left..center)+badge_width+(left~='' and 1 or 0)+(center~='' and 1 or 0)+1
    return left,center,required
  end
  -- Same priority as AgentInfoBar.reflow: context counts, thinking, percentage, model.
  for _,field in ipairs({'context','reasoning','percent','model'}) do
    local _,_,required=sizes()
    if required<=width then break end
    fields[field]=''
  end
  local left,center=sizes()
  if left~='' then left=' '..left end
  local right_start=width-badge_width
  local center_width=vim.fn.strdisplaywidth(center)
  local left_width=vim.fn.strdisplaywidth(left)
  local center_start=center~='' and math.max(left_width+(left~='' and 1 or 0),
    math.min(math.floor((width-center_width)/2),right_start-1-center_width)) or right_start
  local text=left..string.rep(' ',math.max(0,center_start-left_width))..center
  text=text..string.rep(' ',math.max(0,right_start-vim.fn.strdisplaywidth(text)))
  vim.wo[self.input].winbar='%#SosMuted#'..text:gsub('%%','%%%%')
    ..'%#'..(sane and 'SosModeSane' or 'SosModeSlop')..'#'..badge..'%*'
end

function M:update_status()
  if not valid(self.transcript) then return end
  local current=self.session.current
  local label=(not current.kind or current.kind=='discussion') and 'Discussion' or 'Implement'
  local discussion=label=='Implement' and '%#SosMuted# Discussion ' or ''
  vim.wo[self.transcript].winbar=discussion..'%#SosTabActive# '..label..' %#SosNormal#%=%#SosVersion#'..version..' %*'
  self:update_input()
  self:arm_timer()
end

function M:arm_timer()
  if not valid(self.transcript) then return end
  local current=self.session.current
  local activity=current.view and current.view.projection
  local w=self.session.workflow
  local building=self.session.client.transport.state=='ready' and w.state and w.state.status=='running' and not w.archive_view and not w.info.implementation_archive
  if activity and (activity.running_activity or building) and not self.status_timer then
    self.status_timer=true
    vim.defer_fn(function()
      self.status_timer=nil
      self:tick()
    end,150)
  end
end

function M:tick()
  if not valid(self.transcript) then return end
  local current=self.session.current
  if self.displayed~=current then return end
  local view=current.view
  if self.rendering or self.render_pending then self:arm_timer(); return end
  local width=api.nvim_win_get_width(self.transcript)
  if view.width~=width then self:queue_render(); return end
  local changes=Render.refresh_activities(view.projection,width)
  if #changes>0 then
    self.rendering=true
    local position=Scroll.observe(self.transcript,view)
    vim.bo[view.transcript].readonly=false
    vim.bo[view.transcript].modifiable=true
    for _,change in ipairs(changes) do
      api.nvim_buf_set_text(view.transcript,change.line-1,0,change.line-1,#change.previous,{change.text})
    end
    Highlights.apply_lines(view.transcript,view.projection,changes)
    vim.bo[view.transcript].modifiable=false
    vim.bo[view.transcript].readonly=true
    api.nvim_win_call(self.transcript,function() vim.fn.winrestview(position) end)
    view.viewport=Scroll.position(self.transcript)
    self.rendering=false
  end
  self.session.workflow:sync_navigation()
  if current.kind=='review' then self:render_input(current,view) end
  self:arm_timer()
end

function M:render()
  self.render_pending=false
  if not valid(self.transcript) then return end
  if self.displayed and self.displayed.view and api.nvim_win_get_buf(self.transcript)~=self.displayed.view.transcript then self:hide(); return end
  local current = self.session.current
  if (current.kind=='step' or current.kind=='validation') and self.session.workflow.state then
    self.session.workflow:step_summary(current,current.step)
  end
  local switched = self.displayed ~= current
  if switched then self:save_view() end
  local view = self:view(current)
  self:sync_composer(current)
  self.rendering = true
  local position
  local old = view.projection
  if not switched then
    position=Scroll.observe(self.transcript,view)
  else position=view.position end
  local reset=view.fold_reset ~= current.fold_reset
  if reset then
    view.open, view.observed = {}, {}
    view.fold_reset = current.fold_reset
  end
  if api.nvim_win_get_buf(self.transcript)~=view.transcript then api.nvim_win_set_buf(self.transcript,view.transcript) end
  if valid(self.input) and api.nvim_win_get_buf(self.input)~=view.input then api.nvim_win_set_buf(self.input,view.input) end
  -- Switching buffers resets this window-local option to the editor's fold filler.
  vim.wo[self.transcript].fillchars = 'fold: ,foldopen:▾,foldclose:▸,foldsep: '
  local projected=current
  if current.loading_implementation or current.implementation_error then
    projected=vim.tbl_extend('force',current,{records=current.implementation_error
      and {{id='implementation-error',role='construction',content=current.implementation_error,group='SosError'}}
      or {{id='implementation-loading',role='activity',label='Loading implementation',count=0,activities={},_started_at=current.loading_started}}})
  elseif current.kind=='revision' then
    projected=vim.tbl_extend('force',current,{records=vim.deepcopy(current.records)})
    table.insert(projected.records,1,{id='candidate-plan',role='assistant',content=
      'Proposed implementation plan\n\n'..((self.session.workflow.state or {}).proposed_plan_markdown or '')
      ..'\n\n:SosAcceptProposal executes only after explicit confirmation. :SosRejectProposal dismisses the proposal.'})
    if not self.session.workflow:readonly(current) then
      projected.records[#projected.records+1]={id='revision/accept',role='construction',content='[ Accept proposal ]',group='SosAction',control='accept_proposal'}
      projected.records[#projected.records+1]={id='revision/decline',role='construction',content='[ Decline proposal ]',group='SosAction',control='reject_proposal'}
    end
  end
  local state=self.session.workflow.state
  if current.kind=='review' and not self.session.workflow:readonly(current)
    and state and type(state.proposed_rework)=='string' and state.proposed_rework~='' then
    projected=vim.tbl_extend('force',current,{records=vim.list_extend(vim.list_slice(current.records),{
      {id='correction-proposal',role='construction',content='\nCorrection proposal\n\n'..state.proposed_rework},
      {id='accept-proposal',role='construction',content='[ Accept proposal ]',group='SosAction',control='accept_proposal'},
      {id='reject-proposal',role='construction',content='[ Decline proposal ]',group='SosAction',control='reject_proposal'},
    })})
  end
  view.width=api.nvim_win_get_width(self.transcript)
  local projection = Render.project(projected,self.session.info,view.width)
  local refold=switched or reset or not old or not vim.deep_equal(old.levels,projection.levels)
    or not vim.deep_equal(old.spans,projection.spans)
  if refold and not switched and not reset then self:capture_folds(view) end
  Render.bind(view.transcript,projection)
  local lines = projection.lines
  local previous=old and old.lines or api.nvim_buf_get_lines(view.transcript,0,-1,false)
  local first,old_end,new_end=1,#previous,#lines
  while first<=old_end and first<=new_end and previous[first]==lines[first] do first=first+1 end
  while old_end>=first and new_end>=first and previous[old_end]==lines[new_end] do
    old_end,new_end=old_end-1,new_end-1
  end
  local changed=first<=old_end or first<=new_end
  vim.bo[view.transcript].readonly = false
  vim.bo[view.transcript].modifiable = true
  -- Timer-only updates must not replace the prose currently being read or selected.
  if changed then api.nvim_buf_set_lines(view.transcript,first-1,old_end,false,vim.list_slice(lines,first,new_end)) end
  if changed or not old or not vim.deep_equal(old.highlights,projection.highlights)
    or not vim.deep_equal(old.bands,projection.bands) then Highlights.apply(view.transcript,projection) end
  if refold then
    vim.wo[self.transcript].foldmethod = 'expr'
    vim.wo[self.transcript].foldexpr = "v:lua.require'sos.render'.fold(v:lnum)"
    vim.wo[self.transcript].foldenable = true
    vim.wo[self.transcript].foldlevel = 0
    api.nvim_win_call(self.transcript,function()
      vim.cmd('normal! zX')
      -- Parents precede children; never open a child through a closed ancestor.
      for _, span in ipairs(projection.spans) do
        if view.open[span.key] and vim.fn.foldclosed(span.start) == span.start then
          api.nvim_win_set_cursor(self.transcript,{span.start,0})
          vim.cmd('normal! zo')
        end
      end
    end)
  end
  vim.bo[view.transcript].modifiable = false
  vim.bo[view.transcript].readonly = true
  self:render_input(current,view)
  self.displayed = current
  if current.kind=='step' or current.kind=='validation' then
    if position then Scroll.restore(self.transcript,position,old,projection)
    else api.nvim_win_set_cursor(self.transcript,{1,0}) end
    view.viewport=Scroll.position(self.transcript)
  else Scroll.update(self.transcript,view,projection,position,switched,current.history and current.history.page) end
  view.projection = projection
  self:update_status()
  if switched and view.cursor and valid(self.input) then
    pcall(api.nvim_win_set_cursor,self.input,view.cursor)
  end
  if current.focus_control then
    for row,action in pairs(projection.controls) do
      if action==current.focus_control then api.nvim_win_set_cursor(self.transcript,{row,0}); api.nvim_set_current_win(self.transcript); break end
    end
    current.focus_control=nil
  end
  self.rendering = false
  self.session.workflow:sync_navigation()
  require('sos.progress').update(self.session)
end

return M
