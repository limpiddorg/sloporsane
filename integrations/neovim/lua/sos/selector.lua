local M = {}
M.__index=M
function M.open(title,rows)
  local self=setmetatable({rows={},origin=vim.api.nvim_get_current_win()},M)
  self.buffer=vim.api.nvim_create_buf(false,true)
  local width=math.max(20,math.min(80,vim.o.columns-4))
  self.window=vim.api.nvim_open_win(self.buffer,true,{relative='editor',width=width,
    height=math.max(1,math.min(#rows,vim.o.lines-6)),row=2,col=math.max(0,math.floor((vim.o.columns-width)/2)),
    style='minimal',border='single',title=' '..title..' · Enter Select · Esc Close '})
  for _, key in ipairs({'<Esc>','q'}) do vim.keymap.set('n',key,function() self:close() end,{buffer=self.buffer}) end
  vim.keymap.set('n','<CR>',function()
    local row=self.rows[vim.api.nvim_win_get_cursor(self.window)[1]]
    self:close()
    if row and row.choose then row.choose() end
  end,{buffer=self.buffer})
  self:update(rows)
  return self
end
function M:update(rows)
  if not self.window or not vim.api.nvim_win_is_valid(self.window) then return end
  local cursor=vim.api.nvim_win_get_cursor(self.window)[1]
  local key=(self.rows[cursor] or {}).key
  local lines={}
  for _, row in ipairs(rows) do lines[#lines+1]=row.label:gsub('[\r\n]',' ') end
  if #lines==0 then lines={'No entries'} end
  vim.bo[self.buffer].modifiable=true
  vim.api.nvim_buf_set_lines(self.buffer,0,-1,false,lines)
  vim.bo[self.buffer].modifiable=false
  vim.bo[self.buffer].readonly=true
  self.rows=rows
  for n,row in ipairs(rows) do if row.key==key then cursor=n; break end end
  vim.api.nvim_win_set_cursor(self.window,{math.max(1,math.min(cursor,#lines)),0})
end
function M:close()
  if self.window and vim.api.nvim_win_is_valid(self.window) then vim.api.nvim_win_close(self.window,true) end
  if self.buffer and vim.api.nvim_buf_is_valid(self.buffer) then vim.api.nvim_buf_delete(self.buffer,{force=true}) end
  self.window=nil
  if vim.api.nvim_win_is_valid(self.origin) then vim.api.nvim_set_current_win(self.origin) end
end
return M
