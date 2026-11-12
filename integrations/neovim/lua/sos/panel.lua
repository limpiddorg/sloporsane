local M = {}
local api = vim.api
local Render = require('sos.render')
local Highlights = require('sos.highlights')
local function valid(win) return win and api.nvim_win_is_valid(win) end

function M.new(session)
  Highlights.setup()
  local self = setmetatable({session=session}, {__index=M})
  session.on_change = function() self:render() end
  return self
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
  -- Preserve native fold commands, then observe their resulting state.
  for _, command in ipairs({'zo','zO','zc','zC','za','zA','zr','zR','zm','zM','zx','zv'}) do
    vim.keymap.set('n',command,function()
      vim.cmd('normal! '..(vim.v.count > 0 and tostring(vim.v.count) or '')..command)
      self:sync_folds()
    end,{buffer=transcript,silent=true})
  end
  vim.keymap.set('n','<CR>',function()
    if vim.fn.foldlevel('.') > 0 then
      vim.cmd('normal! za')
      self:sync_folds()
    end
  end,{buffer=transcript,silent=true,desc='Toggle SOS Activity or plan fold'})
  api.nvim_create_autocmd({'CursorMoved','WinScrolled','SafeState'}, {
    callback=function()
      if not self.rendering and api.nvim_get_current_buf() == transcript then self:sync_folds() end
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
    api.nvim_win_call(self.transcript, function() view.position = vim.fn.winsaveview() end)
  end
  if valid(self.input) then view.cursor = api.nvim_win_get_cursor(self.input) end
end

function M:hide()
  if self.session.inspector then self.session.inspector:hide() end
  self:save_view()
  if valid(self.input) then api.nvim_win_close(self.input, true) end
  if valid(self.transcript) then api.nvim_win_close(self.transcript, true) end
  self.input, self.transcript = nil, nil
end

function M:show()
  if valid(self.transcript) and valid(self.input) then return end
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
  api.nvim_win_set_height(self.input, 5)
  vim.wo[self.input].wrap = true
  vim.wo[self.transcript].wrap = true
  vim.wo[self.input].winhighlight = 'Normal:SosDraft,NormalNC:SosDraft'
  vim.wo[self.transcript].winhighlight = 'Normal:SosNormal,NormalNC:SosNormal,Folded:SosActivity,FoldColumn:SosActivity'
  vim.wo[self.transcript].foldminlines = 0
  vim.wo[self.transcript].foldcolumn = '1'
  vim.wo[self.transcript].fillchars = 'fold: ,foldopen:▾,foldclose:▸,foldsep: '
  vim.wo[self.transcript].foldtext = "v:lua.require'sos.render'.foldtext()"
  self.displayed = nil
  self:render()
  if view.position then
    api.nvim_win_call(self.transcript,function() vim.fn.winrestview(view.position) end)
  end
  if view.cursor then pcall(api.nvim_win_set_cursor,self.input,view.cursor) end
  if valid(origin) then api.nvim_set_current_win(origin) end
end

function M:toggle()
  if valid(self.transcript) then self:hide() else self:show() end
end

function M:update_status()
  if not valid(self.transcript) then return end
  local current=self.session.current
  local status,running=self.session.workflow:progress()
  if next(self.session.permissions or {}) then status,running='Waiting for permission · /permissions',false end
  local frames={'|','/','-','\\'}
  local spinner=running and frames[math.floor(vim.uv.now()/150)%#frames+1]..' ' or ''
  local function escape(text) return text:gsub('[\r\n]',' '):gsub('%%','%%%%') end
  local mode_group=current.mode=='slop' and 'SosSlop' or 'SosSane'
  vim.wo[self.transcript].winbar=' SOS | '..escape(self.session.workflow:label(current))
    ..' | %#'..mode_group..'#'..escape(current.mode)..'%* | '..escape(spinner..status)
  if running and not self.status_timer then
    self.status_timer=true
    vim.defer_fn(function() self.status_timer=nil; self:update_status() end,150)
  end
end

function M:render()
  if not valid(self.transcript) or not valid(self.input) then return end
  local current = self.session.current
  local switched = self.displayed ~= current
  if switched then self:save_view() end
  local view = self:view(current)
  self.rendering = true
  local position, follow
  local old = view.projection
  if not switched then
    api.nvim_win_call(self.transcript, function()
      position = vim.fn.winsaveview()
      follow = vim.fn.line('w$') >= api.nvim_buf_line_count(view.transcript)
    end)
  end
  if view.fold_reset ~= current.fold_reset then
    view.open, view.observed = {}, {}
    view.fold_reset = current.fold_reset
  elseif not switched then self:capture_folds(view) end
  api.nvim_win_set_buf(self.transcript, view.transcript)
  api.nvim_win_set_buf(self.input, view.input)
  local projected=current
  if current.kind=='revision' then
    projected=vim.tbl_extend('force',current,{records=vim.deepcopy(current.records)})
    table.insert(projected.records,1,{id='candidate-plan',role='assistant',content=
      'Proposed implementation plan\n\n'..((self.session.workflow.state or {}).proposed_plan_markdown or '')
      ..'\n\n:SosAcceptProposal executes only after explicit confirmation. :SosRejectProposal dismisses the proposal.'})
  end
  local projection = Render.project(projected,self.session.info)
  local lines = projection.lines
  vim.bo[view.transcript].readonly = false
  vim.bo[view.transcript].modifiable = true
  api.nvim_buf_set_lines(view.transcript, 0, -1, false, lines)
  Highlights.apply(view.transcript,projection)
  vim.b[view.transcript].sos_foldlevels = projection.levels
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
  view.projection = projection
  vim.bo[view.transcript].modifiable = false
  vim.bo[view.transcript].readonly = true
  local draft = table.concat(api.nvim_buf_get_lines(view.input,0,-1,false), '\n')
  if draft ~= current.draft then
    self.updating = true
    vim.bo[view.input].modifiable = true
    api.nvim_buf_set_lines(view.input,0,-1,false,vim.split(current.draft,'\n',{plain=true}))
    self.updating = false
  end
  local readonly=self.session.workflow:readonly(current)
  vim.bo[view.input].modifiable = not readonly
  vim.bo[view.input].readonly = readonly
  self:update_status()
  local info=self.session.workflow.info
  local paths=' Original: '..(info.workspace or current.workspace)
  if type(info.execution_workspace)=='string' and info.execution_workspace~='' then paths=paths..' | Execution: '..info.execution_workspace end
  vim.wo[self.input].winbar = ((readonly and ' Read-only | Finish implementation or switch context |' or ' Draft | :SosSend |')..paths):gsub('%%','%%%%')
  self.displayed = current
  api.nvim_win_call(self.transcript, function()
    if switched and view.position then vim.fn.winrestview(view.position)
    elseif follow then api.nvim_win_set_cursor(self.transcript,{#lines,0})
    elseif position then
      if old then
        local top = old.line_keys[position.topline]
        local cursor = old.line_keys[position.lnum]
        position.topline = projection.anchors[top] or position.topline
        position.lnum = projection.anchors[cursor] or position.lnum
      end
      position.lnum = math.min(position.lnum,#lines)
      vim.fn.winrestview(position)
    end
  end)
  if switched and view.cursor then
    pcall(api.nvim_win_set_cursor,self.input,view.cursor)
  end
  self.rendering = false
end

return M
