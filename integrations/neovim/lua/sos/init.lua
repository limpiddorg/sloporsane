local M = {}
local client
local session
local panel

function M.panel()
  if not panel then panel = require('sos.panel').new(M.session()) end
  return panel
end

function M.action(name)
  if name == 'toggle' then
    local p=M.panel()
    p:toggle()
    -- Opening fetches saved defaults without creating a session. Reconnection
    -- after an established connection is still explicit, never a toggle side effect.
    if p.input and not M.session().ever_connected then M.connect() end
  elseif name == 'send' then
    local s = M.session()
    if s.current.draft:match('^/permissions%s*$') then require('sos.actions').permission(s); return end
    if s.workflow:readonly(s.current) then s:notice('This context is read-only; use :SosContext'); return end
    if s.current.draft:match('^/') then require('sos.actions').slash(s)
    else s:send() end
  else
    local navigation={steps=true,inspect=true,next_substep=true,previous_substep=true,next_change=true,
      previous_change=true,explanation=true,before=true}
    if not navigation[name] then M.panel():show() end
    local s = M.session()
    s[name](s)
  end
end

function M.session()
  if not session then session = require('sos.session').new(M.client(), M.options) end
  return session
end
local defaults = {
  socket = '/tmp/sosd.sock',
  daemon = nil,
  auto_start = true,
  timeout_ms = 10000,
  python = 'python3',
}
M.options = vim.deepcopy(defaults)

function M.setup(options)
  assert(not client, 'Configure SOS before connecting')
  assert(vim.fn.has('nvim-0.10') == 1, 'SOS requires Neovim 0.10+')
  M.options = vim.tbl_extend('force', defaults, options or {})
  for action, lhs in pairs(M.options.mappings or {}) do
    if vim.fn.maparg(lhs, 'n') == '' then
      vim.keymap.set('n', lhs, '<Plug>(Sos' .. action .. ')', {remap=true})
    else vim.notify('SOS: existing mapping preserved: ' .. lhs, vim.log.levels.WARN) end
  end
end

function M.client()
  if not client then
    client = require('sos.client').new(M.options)
  end
  return client
end

function M.connect()
  M.session().client:connect()
end

function M.disconnect()
  if client then client:close() end
end

return M
