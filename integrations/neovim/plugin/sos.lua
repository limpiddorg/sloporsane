if vim.g.loaded_sos then return end
vim.g.loaded_sos = true
require('sos.highlights').setup()
vim.api.nvim_create_autocmd('ColorScheme', {
  group = vim.api.nvim_create_augroup('SosHighlights', {clear=true}),
  callback = function() require('sos.highlights').setup() end,
})
vim.api.nvim_create_user_command('SosConnect', function() require('sos').connect() end, {})
vim.api.nvim_create_user_command('SosReconnect', function() require('sos').session().client:reconnect() end, {})
vim.api.nvim_create_user_command('SosDisconnect', function() require('sos').disconnect() end, {})
local actions = {New='new_session',Resume='resume',Toggle='toggle',Send='send',Cancel='cancel',Mode='toggle_mode',
   Older='older',Newer='newer',Latest='latest',Context='contexts',Plan='plan',AutoPlan='auto_plan',
   Start='start',RetryPlan='retry_plan',ImplementationPlan='implementation_plan',GeneratePlan='generate_plan',
   Steps='steps',Continue='continue',Inspect='inspect',NextSubstep='next_substep',PreviousSubstep='previous_substep',
   NextChange='next_change',PreviousChange='previous_change',Explanation='explanation',Indicator='indicator',Validate='validate',
   Review='review_question',Proposal='proposal',AcceptProposal='accept_proposal',RejectProposal='reject_proposal',
   Revision='revision_plan',Versions='versions'}
for name, action in pairs(actions) do
  local run = function() require('sos').action(action) end
  vim.api.nvim_create_user_command('Sos' .. name, run, {})
  vim.keymap.set('n', '<Plug>(Sos' .. name .. ')', run, {silent=true})
end
vim.api.nvim_create_autocmd('VimLeavePre', {
  callback = function() require('sos').disconnect() end,
})
