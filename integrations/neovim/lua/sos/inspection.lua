local Changes=require('sos.changes')
local Source=require('sos.inspection_source')
local api=vim.api
local M={}
M.__index=M
local function valid(win) return win and api.nvim_win_is_valid(win) end
local function owned(win,buffer) return valid(win) and api.nvim_win_get_buf(win)==buffer end
local serial=0
function M.new(session)
  local self=setmetatable({session=session,model=Changes.new(),collapsed=false},M)
  self.group=api.nvim_create_augroup('SosInspection'..tostring(vim.uv.hrtime()),{clear=true})
  api.nvim_create_autocmd({'FocusGained','BufEnter','CursorHold'}, {group=self.group,callback=function() self:recheck() end})
  return self
end
function M:buffer(kind)
  local buffer=api.nvim_create_buf(false,true)
  serial=serial+1
  api.nvim_buf_set_name(buffer,'sos-inspection://'..kind..'/'..serial)
  vim.bo[buffer].buftype='nofile'
  vim.bo[buffer].bufhidden='hide'
  vim.bo[buffer].swapfile=false
  vim.bo[buffer].undolevels=-1
  for key,action in pairs({[']s']='next_substep',['[s']='previous_substep',[']c']='next_change',['[c']='previous_change',
    ['gx']='toggle_explanation',['q']='hide'}) do
    vim.keymap.set('n',key,function() self[action](self) end,{buffer=buffer,silent=true})
  end
  return buffer
end
function M:put(buffer,lines)
  vim.bo[buffer].readonly=false
  vim.bo[buffer].modifiable=true
  api.nvim_buf_set_lines(buffer,0,-1,false,#lines>0 and lines or {''})
  vim.bo[buffer].modified=false
  vim.bo[buffer].modifiable=false
  vim.bo[buffer].readonly=true
end
function M:show()
  local w=self.session.workflow
  if not w.state or not w.selected_identity then self.session:notice('Select an implementation step first'); return end
  local index
  for n,step in ipairs(w.state.steps) do if step.identity==w.selected_identity then index=n-1; break end end
  if not index then self:hide(); return end
  self.model:bind(w.branch_id..'/'..(w.archive_key or 'live'),w.state,index)
  self.visible=true
  self:windows()
  self:render()
  self:watch()
end
function M:windows()
  local focus=api.nvim_get_current_win()
  if not owned(self.code,self.code_buffer) then
    self.code_buffer=self:buffer('code')
    local origin=self.session.panel_origin
    if not valid(origin) then origin=focus end
    api.nvim_win_call(origin,function()
      vim.cmd('leftabove vsplit')
      self.code=api.nvim_get_current_win()
      api.nvim_win_set_buf(self.code,self.code_buffer)
    end)
    vim.wo[self.code].number=true
    vim.wo[self.code].signcolumn='yes'
    vim.wo[self.code].wrap=false
  end
  if not self.collapsed and not owned(self.explanation,self.explanation_buffer) then
    self.explanation_buffer=self:buffer('explanation')
    api.nvim_win_call(self.code,function()
      vim.cmd('aboveleft 5split')
      self.explanation=api.nvim_get_current_win()
      api.nvim_win_set_buf(self.explanation,self.explanation_buffer)
    end)
    vim.bo[self.explanation_buffer].filetype='markdown'
  end
  if valid(focus) then api.nvim_set_current_win(focus) end
end
function M:hide()
  self.visible=false
  for _,pair in ipairs({{self.explanation,self.explanation_buffer},{self.code,self.code_buffer}}) do
    if owned(pair[1],pair[2]) and #api.nvim_list_wins()>1 then api.nvim_win_close(pair[1],true) end
  end
  self.code,self.explanation=nil,nil
end
function M:toggle_explanation()
  self.collapsed=not self.collapsed
  if self.collapsed and owned(self.explanation,self.explanation_buffer) then api.nvim_win_close(self.explanation,true); self.explanation=nil end
  self:windows(); self:render()
end
function M:next_substep() self.model:substep(1); self:render() end
function M:previous_substep() self.model:substep(-1); self:render() end
function M:next_change() self.model:move(1); self:render() end
function M:previous_change() self.model:move(-1); self:render() end
function M:render()
  if not self.visible or not owned(self.code,self.code_buffer) then return end
  local model=self.model
  local selected=model:selected()
  self.live=nil
  local source=selected and Source.resolve(selected.diff,self.session.workflow.info.execution_workspace,
    self.session.workflow.info.implementation_archive or model.state.status=='archived')
    or {kind='empty',label='Preserved boundary · No changes',text='No changes'}
  self.source=source
  local lines=vim.split(source.text,'\n',{plain=true})
  if #lines>1 and lines[#lines]=='' then table.remove(lines) end
  self:put(self.code_buffer,lines)
  local ft=selected and vim.filetype.match({filename=selected.path}) or nil
  vim.bo[self.code_buffer].filetype=(source.kind=='binary' or source.kind=='empty') and '' or ft or ''
  local header=model:heading()..' | '..source.label
  vim.wo[self.code].winbar=header:gsub('[\r\n]',' '):gsub('%%','%%%%')
  if owned(self.explanation,self.explanation_buffer) then
    self:put(self.explanation_buffer,model:explanation())
    vim.wo[self.explanation].winbar=(' Explanation | '..model:heading()..' | gx Collapse'):gsub('%%','%%%%')
  end
  self:annotate(selected,source,#lines)
  if source.kind=='live' then self.live={diff=selected.diff,path=source.path} end
end
function M:annotate(block,source,count)
  self.namespace=self.namespace or api.nvim_create_namespace('sos-inspection')
  api.nvim_buf_clear_namespace(self.code_buffer,self.namespace,0,-1)
  if not block or source.kind=='binary' then return end
  local row=source.side=='before' and block.old_start or block.new_start
  for _,line in ipairs(block.lines) do
    if line:sub(1,1)==' ' then row=row+1
    elseif line:sub(1,1)=='+' or line:sub(1,1)=='-' then break end
  end
  local target=math.max(1,math.min(row,count))
  api.nvim_win_set_cursor(self.code,{target,0})
  local new=block.new_start
  local before=block.old_start
  for _,line in ipairs(block.lines) do
    local prefix=line:sub(1,1)
    if prefix=='+' then
      if source.side~='before' then self:mark(new,count,'DiffAdd','+') end
      new=new+1
    elseif prefix=='-' then
      if source.side=='before' then self:mark(before,count,'DiffDelete','-')
      else self:mark(new,count,'DiffChange','- deleted') end
      before=before+1
    elseif prefix==' ' then new=new+1; before=before+1 end
  end
end
function M:mark(line,count,group,label)
  api.nvim_buf_set_extmark(self.code_buffer,self.namespace,math.max(0,math.min(line-1,count-1)),0,
    {line_hl_group=group,virt_text={{label,group}},virt_text_pos='eol'})
end
function M:recheck()
  if not self.visible or not self.live or not owned(self.code,self.code_buffer) then return end
  local saved=self.live
  local current=Source.resolve(saved.diff,self.session.workflow.info.execution_workspace,
    self.session.workflow.info.implementation_archive or self.model.state.status=='archived')
  if current.kind~='live' or current.path~=saved.path then
    -- Render only preserved bytes; successor contents never enter this buffer.
    self:render()
  end
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
  if self.session.current.kind~='step' or not w.state then self:hide(); return end
  local index
  for n,step in ipairs(w.state.steps) do if step.identity==w.selected_identity then index=n-1; break end end
  if not index then self:hide(); return end
  local before=self.model.item and vim.deepcopy(self.model.item)
  local key=self.model.key
  self.model:bind(w.branch_id..'/'..(w.archive_key or 'live'),w.state,index)
  if key~=self.model.key or not vim.deep_equal(before,self.model.item) then self:render() else self:recheck() end
end
return M
