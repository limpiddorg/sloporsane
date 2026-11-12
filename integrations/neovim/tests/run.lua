-- Run from the repository root; all generated artifacts belong in build-ai/.
local root = vim.fn.getcwd()
vim.opt.runtimepath:prepend(root .. '/integrations/neovim')
vim.fn.mkdir(root .. '/build-ai/neovim', 'p')
-- Only transient socket endpoints live outside build-ai/: macOS limits the full path.
local sockets = assert(vim.uv.fs_mkdtemp('/tmp/sos-nvim-XXXXXX'))
vim.env.SOS_TEST_SOCKET_DIR = sockets
local ok, err = xpcall(function()
  dofile(root .. '/integrations/neovim/tests/transport.lua')
  dofile(root .. '/integrations/neovim/tests/conversations.lua')
  dofile(root .. '/integrations/neovim/tests/history.lua')
  dofile(root .. '/integrations/neovim/tests/appearance.lua')
  dofile(root .. '/integrations/neovim/tests/workflow.lua')
  dofile(root .. '/integrations/neovim/tests/inspection.lua')
  dofile(root .. '/integrations/neovim/tests/review.lua')
end, debug.traceback)
vim.fn.delete(sockets, 'rf')
vim.env.SOS_TEST_SOCKET_DIR = nil
if not ok then
  io.stderr:write(err .. '\n')
  vim.cmd('cquit 1')
else
  print('SOS Neovim tests passed')
  vim.cmd('qa!')
end
