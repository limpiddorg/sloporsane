local Changes = require('sos.changes')
local Source = require('sos.inspection_source')
local Layout = require('sos.layout')
local api = vim.api
local M = {}
M.__index = M
local function valid(win) return win and api.nvim_win_is_valid(win) end
local function owned(win, buffer) return valid(win) and api.nvim_win_get_buf(win) == buffer end

function M.new(session)
  local self = setmetatable({session=session, model=Changes.new(), collapsed=false, buffers={}}, M)
  self.group = api.nvim_create_augroup('SosInspection'..tostring(vim.uv.hrtime()), {clear=true})
  api.nvim_create_autocmd({'FocusGained','BufEnter','CursorHold'}, {group=self.group, callback=function() self:recheck() end})
  return self
end
function M:configure(buffer)
  self.buffers[buffer] = true
  for key, action in pairs({[']s']='next_substep', ['[s']='previous_substep', [']c']='next_change', ['[c']='previous_change',
    ['gx']='toggle_explanation', ['gb']='toggle_before', ['q']='hide'}) do
    vim.keymap.set('n', key, function() self[action](self) end, {buffer=buffer, silent=true})
  end
  vim.bo[buffer].swapfile = false
  vim.bo[buffer].modeline = false
  vim.bo[buffer].readonly = true
  vim.bo[buffer].modifiable = false
end
function M:buffer(kind)
  local buffer = api.nvim_create_buf(false, true)
  api.nvim_buf_set_name(buffer, kind=='code' and 'sos-inspection' or 'sos-inspection-'..kind)
  vim.bo[buffer].bufhidden = 'hide'
  vim.bo[buffer].undolevels = -1
  vim.bo[buffer].autoread = false
  vim.bo[buffer].filetype = kind=='code' and 'sos_inspection' or 'sos_inspection_'..kind
  self:configure(buffer)
  return buffer
end
function M:put(buffer, lines)
  vim.bo[buffer].readonly = false
  vim.bo[buffer].modifiable = true
  api.nvim_buf_set_lines(buffer, 0, -1, false, #lines > 0 and lines or {''})
  vim.bo[buffer].modified = false
  vim.bo[buffer].modifiable = false
  vim.bo[buffer].readonly = true
end
function M:can_open(origin)
  return (owned(self.code,self.code_buffer) and (origin.window==self.code or origin.window==self.explanation or origin.window==self.header))
    or (valid(origin.window) and api.nvim_win_get_buf(origin.window)==origin.buffer)
end
function M:show(focus,origin)
  local w = self.session.workflow
  if not w.state or not w.selected_identity then self.session:notice('Select an implementation step first'); return end
  local index
  for n, step in ipairs(w.state.steps) do if step.identity == w.selected_identity then index=n-1; break end end
  if not index then self:hide(); return end
  origin=origin or self.session:inspection_origin()
  local reuse=owned(self.code,self.code_buffer) and (origin.window==self.code or origin.window==self.explanation or origin.window==self.header
    or (origin.from_chat and not self.borrowed))
  if not reuse then
    if self.visible then self:hide() end
  end
  self.placement=origin
  self.model:bind(w.branch_id..'/'..(w.archive_key or 'live'), w.state, index)
  self.visible = true
  self:windows()
  self:render()
  self:watch()
  if focus then vim.cmd('stopinsert'); self:focus() end
end
function M:focus()
  if owned(self.code,self.code_buffer) then api.nvim_set_current_win(self.code) end
end
function M:windows()
  local focus = api.nvim_get_current_win()
  local sizes=Layout.heights({[owned(self.code,self.code_buffer) and self.code or -1]=true,
    [owned(self.header,self.header_buffer) and self.header or -1]=true,
    [owned(self.explanation,self.explanation_buffer) and self.explanation or -1]=true})
  if not owned(self.code, self.code_buffer) then
    if not self.scratch or not api.nvim_buf_is_valid(self.scratch) then self.scratch=self:buffer('code') end
    self.code_buffer = self.scratch
    local placement=self.placement or self.session:inspection_origin()
    local replace=valid(placement.window) and not placement.from_chat
      and api.nvim_win_get_buf(placement.window)==placement.buffer
      and api.nvim_win_get_config(placement.window).relative==''
      and not vim.tbl_contains({'wipe','delete','unload'},vim.bo[placement.buffer].bufhidden)
    if replace then
      local options={}
      for _,name in ipairs({'number','signcolumn','wrap','winbar','statusline','foldenable'}) do options[name]=vim.wo[placement.window][name] end
      self.borrowed={window=placement.window,buffer=placement.buffer,options=options,
        view=api.nvim_win_call(placement.window,vim.fn.winsaveview)}
      self.code=placement.window
      api.nvim_win_call(self.code,function() vim.cmd('keepalt hide buffer '..self.code_buffer) end)
    else
      self.borrowed=nil
      local origin=self.session.panel_origin
      if not valid(origin) then origin=placement.window end
      api.nvim_win_call(origin,function()
        vim.cmd('leftabove vsplit')
        self.code=api.nvim_get_current_win()
        api.nvim_win_set_buf(self.code,self.code_buffer)
      end)
    end
    vim.wo[self.code].number = true
    vim.wo[self.code].signcolumn = 'yes'
    vim.wo[self.code].wrap = false
    vim.wo[self.code].foldenable = false
    vim.wo[self.code].winbar = ''
    vim.wo[self.code].statusline = ' '
  end
  if not owned(self.header,self.header_buffer) then
    if not self.header_buffer or not api.nvim_buf_is_valid(self.header_buffer) then self.header_buffer=self:buffer('header') end
    api.nvim_win_call(self.code,function()
      vim.cmd('aboveleft 2split')
      self.header=api.nvim_get_current_win()
      api.nvim_win_set_buf(self.header,self.header_buffer)
    end)
    self:plain_window(self.header)
    vim.wo[self.header].winfixheight=true
  end
  if not self.collapsed and not owned(self.explanation, self.explanation_buffer) then
    if not self.explanation_buffer or not api.nvim_buf_is_valid(self.explanation_buffer) then
      self.explanation_buffer = self:buffer('explanation')
    end
    api.nvim_win_call(self.header, function()
      vim.cmd('aboveleft 5split')
      self.explanation = api.nvim_get_current_win()
      api.nvim_win_set_buf(self.explanation, self.explanation_buffer)
    end)
    self:plain_window(self.explanation)
    vim.wo[self.explanation].wrap=true
    vim.bo[self.explanation_buffer].syntax = 'markdown'
  end
  if valid(focus) then api.nvim_set_current_win(focus) end
  Layout.restore_heights(sizes)
end
function M:plain_window(win)
  vim.wo[win].number=false
  vim.wo[win].relativenumber=false
  vim.wo[win].signcolumn='no'
  vim.wo[win].foldcolumn='0'
  vim.wo[win].foldenable=false
  vim.wo[win].wrap=false
  vim.wo[win].winbar=''
  vim.wo[win].statusline=' '
  vim.wo[win].statuscolumn=''
end
function M:hide()
  local sizes=Layout.heights({[owned(self.code,self.code_buffer) and self.code or -1]=true,
    [owned(self.header,self.header_buffer) and self.header or -1]=true,
    [owned(self.explanation,self.explanation_buffer) and self.explanation or -1]=true})
  self.visible = false
  if owned(self.explanation,self.explanation_buffer) then api.nvim_win_close(self.explanation,true) end
  if owned(self.header,self.header_buffer) then api.nvim_win_close(self.header,true) end
  if owned(self.code,self.code_buffer) then
    if self.borrowed and self.borrowed.window==self.code then
      local previous=self.borrowed
      local buffer=api.nvim_buf_is_valid(previous.buffer) and previous.buffer or api.nvim_create_buf(true,false)
      api.nvim_win_set_buf(self.code,buffer)
      for name,value in pairs(previous.options) do vim.wo[self.code][name]=value end
      api.nvim_win_call(self.code,function() vim.fn.winrestview(previous.view) end)
    elseif #api.nvim_list_wins()>1 then api.nvim_win_close(self.code,true)
    else api.nvim_win_set_buf(self.code,api.nvim_create_buf(true,false)) end
  end
  self.borrowed=nil
  self.code, self.header, self.explanation = nil, nil, nil
  -- Release hidden display buffers so reopening can retain their simple names.
  for buffer in pairs(self.buffers) do
    if api.nvim_buf_is_valid(buffer) and #vim.fn.win_findbuf(buffer)==0 then api.nvim_buf_delete(buffer,{force=true}) end
    if not api.nvim_buf_is_valid(buffer) then self.buffers[buffer]=nil end
  end
  for _,field in ipairs({'scratch','header_buffer','explanation_buffer'}) do
    if self[field] and not api.nvim_buf_is_valid(self[field]) then self[field]=nil end
  end
  if not self.scratch then self.highlighted_type=nil end
  Layout.restore_heights(sizes)
end
function M:toggle_explanation()
  self.collapsed = not self.collapsed
  if self.collapsed and owned(self.explanation, self.explanation_buffer) then
    api.nvim_win_close(self.explanation, true); self.explanation=nil
  end
  self:windows(); self:render(true)
end
function M:toggle_before() self.before=not self.before; self:render() end
function M:navigate(kind,delta)
  self.before=false
  self.model[kind](self.model,delta)
  self:render()
  local workflow=self.session.workflow
  if self.session.current.kind=='review' and not workflow:readonly(self.session.current) then
    -- Navigation changes the question's local file/hunk context, never starts a
    -- review turn, validates an indicator or makes a daemon request.
    workflow:review(self.model.index,self.model.part,false)
  end
  self:focus()
end
function M:next_substep() self:navigate('substep',1) end
function M:previous_substep() self:navigate('substep',-1) end
function M:next_change() self:navigate('move',1) end
function M:previous_change() self:navigate('move',-1) end

local function lines(text)
  local result=vim.split(text, '\n', {plain=true})
  if #result > 1 and result[#result] == '' then table.remove(result) end
  return result
end
function M:highlight(filetype)
  if self.highlighted_type==filetype then return end
  self.highlighted_type=filetype
  local buffer=self.code_buffer
  pcall(vim.treesitter.stop,buffer)
  -- Use the user's runtime syntax, queries and highlight groups, not language
  -- FileType/BufRead hooks, ftplugins or language-server attachment.
  vim.bo[buffer].syntax=filetype
  if filetype=='' then return end
  local language=vim.treesitter.language.get_lang(filetype) or filetype
  local ok,configs=pcall(require,'nvim-treesitter.configs')
  if ok and not configs.is_enabled('highlight',language,buffer) then return end
  local queried,query=pcall(vim.treesitter.query.get,language,'highlights')
  if not queried or not query then return end
  local started=pcall(vim.treesitter.start,buffer,language)
  local regex=ok and configs.get_module('highlight').additional_vim_regex_highlighting
  if started and not (regex==true or (type(regex)=='table' and vim.tbl_contains(regex,language))) then vim.bo[buffer].syntax='' end
end
function M:render(preserve_view)
  if not self.visible or not owned(self.code, self.code_buffer) then return end
  local model=self.model
  local selected=model:selected()
  local selection={key=model.key,part=model.part,path=selected and selected.path,hunk=selected and selected.hunk,before=self.before==true}
  local position
  if preserve_view and vim.deep_equal(selection,self.rendered_selection) then
    position=api.nvim_win_call(self.code,vim.fn.winsaveview)
  end
  self.live=nil
  local source=selected and Source.resolve(selected.diff, self.session.workflow.info.execution_workspace,
    self.session.workflow.info.implementation_archive or model.state.status=='archived', self.buffers)
    or {kind='empty', label='Preserved boundary · No changes', text='No changes'}
  if self.before and selected and source.kind~='binary' then
    source={kind='historical', side='before', label='Historical · Before · '..selected.path, text=selected.diff.before or ''}
  end
  local buffer=self.code_buffer
  self:put(buffer, lines(source.text))
  self.namespace=self.namespace or api.nvim_create_namespace('sos-inspection')
  self.source=source
  local ft=selected and vim.filetype.match({filename=selected.path}) or nil
  self:highlight((source.kind=='binary' or source.kind=='empty') and '' or ft or '')
  vim.bo[buffer].readonly=true
  vim.bo[buffer].modifiable=false
  local heading=model:heading()
  if owned(self.header,self.header_buffer) then
    self:put(self.header_buffer,{heading,selected and selected.path:gsub('[\r\n]',' ') or 'No changes'})
    api.nvim_win_set_height(self.header,2)
  end
  if owned(self.explanation, self.explanation_buffer) then
    self:put(self.explanation_buffer, model:explanation())
    vim.wo[self.explanation].winbar='Explanation'
  end
  self:annotate(selected, source, api.nvim_buf_line_count(buffer))
  if position then api.nvim_win_call(self.code,function() vim.fn.winrestview(position) end) end
  self.rendered_selection=selection
  if source.kind=='live' then self.live={diff=selected.diff,path=source.path} end
end
function M:annotate(block, source, count)
  api.nvim_buf_clear_namespace(self.code_buffer, self.namespace, 0, -1)
  if not block or source.kind=='binary' then return end
  local row=source.side=='before' and block.old_start or block.new_start
  for _, line in ipairs(block.lines) do
    if line:sub(1,1)==' ' then row=row+1
    elseif line:sub(1,1)=='+' or line:sub(1,1)=='-' then break end
  end
  api.nvim_win_set_cursor(self.code, {math.max(1,math.min(row,count)),0})
  local new, before=block.new_count==0 and block.new_start+1 or block.new_start, block.old_start
  local removed={}
  local function flush()
    if #removed==0 then return end
    local anchor=math.max(0,math.min(new-1,count-1))
    api.nvim_buf_set_extmark(self.code_buffer,self.namespace,anchor,0,{
      virt_lines=removed, virt_lines_above=new<=count,
    })
    removed={}
  end
  for _, line in ipairs(block.lines) do
    local prefix=line:sub(1,1)
    if prefix=='+' then
      flush()
      if source.side~='before' then self:mark(new,count,'DiffAdd','+') end
      new=new+1
    elseif prefix=='-' then
      if source.side=='before' then self:mark(before,count,'DiffDelete','-')
      else removed[#removed+1]={{'- '..line:sub(2),'DiffDelete'}} end
      before=before+1
    elseif prefix==' ' then flush(); new=new+1; before=before+1 end
  end
  flush()
end
function M:mark(line, count, group, label)
  api.nvim_buf_set_extmark(self.code_buffer,self.namespace,math.max(0,math.min(line-1,count-1)),0,{
    line_hl_group=group,sign_text=label,sign_hl_group=group,
  })
end
function M:recheck()
  if self.checking or not self.visible or not self.live or not owned(self.code,self.code_buffer) then return end
  self.checking=true
  local saved=self.live
  local current=Source.resolve(saved.diff,self.session.workflow.info.execution_workspace,
    self.session.workflow.info.implementation_archive or self.model.state.status=='archived',self.buffers)
  if current.kind~='live' or current.path~=saved.path or vim.bo[self.code_buffer].modified then self:render(true) end
  self.checking=false
end
function M:watch()
  if self.watching then return end
  self.watching=true
  vim.defer_fn(function()
    self.watching=false
    if self.visible and owned(self.code,self.code_buffer) then self:recheck(); self:watch() end
  end,500)
end
function M:update()
  if not self.visible then return end
  local w=self.session.workflow
  local current=self.session.current
  if not w.state or (current.kind~='step' and current.kind~='review') then self:hide(); return end
  if current.kind=='review' and current.review_key~=w:review_key(current.step) then self:recheck(); return end
  local index
  for n,step in ipairs(w.state.steps) do if step.identity==w.selected_identity then index=n-1; break end end
  if not index then self:hide(); return end
  local before=self.model.item and vim.deepcopy(self.model.item)
  local key=self.model.key
  self.model:bind(w.branch_id..'/'..(w.archive_key or 'live'),w.state,index)
  if key~=self.model.key or not vim.deep_equal(before,self.model.item) then self:render(true) else self:recheck() end
end
return M
