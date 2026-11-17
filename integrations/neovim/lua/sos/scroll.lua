-- Follow conversation text, not Activity growth, while native navigation owns reading.
local M = {}
local api = vim.api

function M.position(win)
  return api.nvim_win_call(win,vim.fn.winsaveview)
end

local function at_bottom(win,projection,position)
  if projection.user and position.topline<projection.user.line then return false end
  return api.nvim_win_text_height(win,{start_row=position.topline-1,start_vcol=position.skipcol}).all
    <=api.nvim_win_get_height(win)
end

function M.observe(win,view)
  local position=M.position(win)
  if view.viewport and not vim.deep_equal(position,view.viewport) and view.projection then
    local last=view.projection.lines[#view.projection.lines]
    local lastcol=math.max(0,vim.fn.byteidx(last,vim.fn.strchars(last)-1))
    local focused=api.nvim_get_current_win()==win
    local scrolled=position.topline~=view.viewport.topline or position.skipcol~=view.viewport.skipcol
    local cursor_end=position.lnum==#view.projection.lines and position.col>=lastcol
      or (focused and vim.fn.foldclosedend(position.lnum)==#view.projection.lines)
    view.follow=at_bottom(win,view.projection,position)
      and (not focused or scrolled or cursor_end)
  end
  view.viewport=position
  return position
end

function M.restore(win,position,old,projection)
  position=vim.deepcopy(position)
  if old then
    local function anchor(row)
      local found=projection.anchors[old.line_keys[row]]
      if found then return found end
      if old.user and projection.user and old.user.key~=projection.user.key
        and old.user.record==projection.user.record and row>=old.user.line then
        return projection.user.line+row-old.user.line
      end
      return row
    end
    position.topline=anchor(position.topline)
    position.lnum=anchor(position.lnum)
  end
  position.lnum=math.min(position.lnum,#projection.lines)
  position.topline=math.min(position.topline,#projection.lines)
  api.nvim_win_call(win,function() vim.fn.winrestview(position) end)
end

local function tail(win,projection)
  api.nvim_win_call(win,function()
    -- Native screen-line positioning accounts for wrapping and closed folds.
    api.nvim_win_set_cursor(win,{#projection.lines,0})
    vim.cmd('normal! $')
    local height=api.nvim_win_get_height(win)
    local rows=api.nvim_win_text_height(win,{start_row=#projection.lines-1}).all
    if rows<=height then
      vim.cmd('normal! zb')
    else
      -- Background windows do not resolve cursor-driven smooth scrolling on redraw.
      -- Ask Neovim for the first wrapped row that fits, rather than measuring text ourselves.
      local low,high=0,rows*api.nvim_win_get_width(win)
      while low<high do
        local middle=math.floor((low+high)/2)
        if api.nvim_win_text_height(win,{start_row=#projection.lines-1,start_vcol=middle}).all<=height then high=middle
        else low=middle+1 end
      end
      local position=vim.fn.winsaveview()
      position.topline,position.skipcol=#projection.lines,low
      vim.fn.winrestview(position)
    end
    local position=vim.fn.winsaveview()
    if projection.user and position.topline<projection.user.line then
      position.topline,position.skipcol=projection.user.line,0
      vim.fn.winrestview(position)
    end
  end)
end

function M.update(win,view,projection,position,switched,page)
  local old=view.projection
  local latest=not (page and page.hasNewer)
  local paging=old and view.page and view.page~=page
  local user=projection.user
  -- The optimistic user record adopts its daemon ID in place. That echo is not a send.
  local sent=not paging and user and old and (not old.user
    or (user.key~=old.user.key and user.record~=old.user.record))
  local text_changed=not old or not vim.deep_equal(old.responses,projection.responses)
  local reading=api.nvim_get_current_win()==win
  local resized=view.height~=api.nvim_win_get_height(win) or (old and old.width~=projection.width)
  if latest and (sent or paging or (not position and switched)
    or (view.follow and (text_changed or resized) and not reading)) then
    tail(win,projection)
    view.follow=true
  elseif position then
    M.restore(win,position,old,projection)
  end
  view.page=page
  view.height=api.nvim_win_get_height(win)
  view.viewport=M.position(win)
  if view.follow==nil then view.follow=latest and at_bottom(win,projection,view.viewport) end
end

function M.page(win,view,direction)
  api.nvim_win_call(win,function()
    local key=api.nvim_replace_termcodes(direction=='up' and '<C-b>' or '<C-f>',true,false,true)
    vim.cmd('normal! '..key)
  end)
  M.observe(win,view)
end

return M
