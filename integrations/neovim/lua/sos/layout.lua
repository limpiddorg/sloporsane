-- Native splits may equalize unrelated columns. Preserve their human-sized heights.
local M={}
local api=vim.api
function M.heights(excluded)
  local sizes={}
  for _,win in ipairs(api.nvim_tabpage_list_wins(0)) do
    if not (excluded or {})[win] and api.nvim_win_get_config(win).relative=='' then
      sizes[#sizes+1]={window=win,height=api.nvim_win_get_height(win)}
    end
  end
  return sizes
end
function M.restore_heights(sizes)
  for _,size in ipairs(sizes) do
    if api.nvim_win_is_valid(size.window) and api.nvim_win_get_height(size.window)~=size.height then
      api.nvim_win_set_height(size.window,size.height)
    end
  end
end
return M
