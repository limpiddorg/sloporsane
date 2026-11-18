-- Construction status is only shown below the composer.
local M={}
local api=vim.api
local Surface=require('sos.surface')
function M.close(session)
  local panel=session.panel
  if not panel or not panel.build_surface then return end
  local view=panel.build_surface
  local input=panel.input
  local height=input and api.nvim_win_is_valid(input) and api.nvim_win_get_height(input)
  panel.build_surface=nil
  Surface.close(view)
  if height and api.nvim_win_is_valid(input) then api.nvim_win_set_height(input,height) end
end
function M.update(session)
  local workflow=session.workflow
  local panel=session.panel
  local input=panel and panel.input
  if session.options.progress==false or not input or not api.nvim_win_is_valid(input)
    or not workflow.state or workflow.archive_view or workflow.info.implementation_archive or workflow:finished() then
    M.close(session)
    return
  end
  require('sos.highlights').setup()
  local view=panel.build_surface
  if not Surface.owns(view) or view.anchor~=input then
    M.close(session)
    local height=api.nvim_win_get_height(input)
    view=Surface.new(input,'belowright',1)
    view.anchor=input
    panel.build_surface=view
    api.nvim_win_set_height(input,height)
  end
  local text=workflow:progress(true)
  if next(session.permissions or {}) then text='Waiting for permission · :SosPermissions' end
  Surface.put(view,{(text:gsub('[\r\n]',' '))},'SosActivity')
end
return M
