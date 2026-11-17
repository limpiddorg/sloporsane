-- A nonfocusable status surface remains visible when the conversation is hidden.
local M = {}
local api = vim.api
local function valid(win) return win and api.nvim_win_is_valid(win) end
function M.close(session)
  local view = session.progress_view
  if not view then return end
  if valid(view.window) then api.nvim_win_close(view.window, true) end
  if api.nvim_buf_is_valid(view.buffer) then api.nvim_buf_delete(view.buffer, {force=true}) end
  session.progress_view = nil
end
function M.update(session)
  local workflow = session.workflow
  if session.options.progress == false or not workflow.state or workflow.archive_view
    or workflow.info.implementation_archive or workflow:finished() then M.close(session); return end
  if session.panel and valid(session.panel.transcript) then M.close(session); return end
  require('sos.highlights').setup()
  local text, running = workflow:progress(true)
  if next(session.permissions or {}) then text,running='Waiting for permission · :SosPermissions',false end
  local frames = {'|', '/', '-', '\\'}
  text = ' SOS · '..(running and frames[math.floor(vim.uv.now()/150)%4+1]..' ' or '')..text
  text = text:gsub('[\r\n]', ' ')
  local width = math.max(1, math.min(vim.fn.strdisplaywidth(text)+1, vim.o.columns))
  local view = session.progress_view
  if not view then
    view = {buffer=api.nvim_create_buf(false, true)}
    session.progress_view = view
    vim.bo[view.buffer].bufhidden = 'hide'
    view.window = api.nvim_open_win(view.buffer, false, {
      relative='editor', row=0, col=0, width=width, height=1,
      focusable=false, style='minimal', zindex=40,
    })
    vim.wo[view.window].winhighlight = 'Normal:SosActivity,NormalNC:SosActivity'
  elseif valid(view.window) then
    api.nvim_win_set_config(view.window, {width=width})
  else M.close(session); M.update(session); return end
  vim.bo[view.buffer].modifiable = true
  api.nvim_buf_set_lines(view.buffer, 0, -1, false, {text})
  vim.bo[view.buffer].modifiable = false
  if running and not view.ticking then
    view.ticking = true
    vim.defer_fn(function()
      view.ticking = false
      if session.progress_view == view then M.update(session) end
    end, 150)
  end
end
return M
