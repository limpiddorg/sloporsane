-- Display-only Markdown projection. Stored messages stay intact.
local M = {}
function M.inline(source)
  local text, marks, links = '', {}, {}
  local index = 1
  while index <= #source do
    local rest = source:sub(index)
    local label, url, finish = rest:match('^%[([^%]]+)%]%(([^%)]+)%)()')
    local content, ending = rest:match('^%*%*([^*]+)%*%*()')
    local code, code_end = rest:match('^`([^`]+)`()')
    local emphasis, emphasis_end = rest:match('^%*([^*]+)%*()')
    local group, value, consumed
    if label then group,value,consumed='SosLink',label,finish-1
    elseif content then group,value,consumed='SosBold',content,ending-1
    elseif code then group,value,consumed='SosCode',code,code_end-1
    elseif emphasis then group,value,consumed='SosEmphasis',emphasis,emphasis_end-1 end
    if value then
      local start=#text
      text=text..value
      marks[#marks+1]={group=group,first=start,last=#text}
      if label then links[#links+1]={first=start,last=#text,url=url} end
      index=index+consumed
    else text=text..source:sub(index,index); index=index+1 end
  end
  return text,marks,links
end
function M.project(source)
  local result, fence, widths = {}, nil, {}
  for _, raw in ipairs(vim.split(source or '', '\n', {plain=true})) do
    local delimiter, language=raw:match('^%s*([`~][`~][`~]+)%s*([%w_+.-]*)%s*$')
    local row={marks={},links={}}
    if fence then
      if delimiter and delimiter:sub(1,1)==fence and language=='' then
        fence=nil; row.text=''; row.group='SosCode'
      else row.text='  '..raw; row.group='SosCode' end
    elseif delimiter then
      fence=delimiter:sub(1,1); row.text='  '..(language~='' and language or 'Code'); row.group='SosCodeTitle'
    elseif raw:match('^%s*|?%s*:?[%-][%-][%-]+:?.*|') and not raw:find('[^%s|:%-]') then
      row.text=''; row.group='SosMuted'
    elseif raw:match('^%s*|.*|%s*$') then
      row.cells={}
      for cell in (raw:match('^%s*|(.*)|%s*$')..'|'):gmatch('(.-)|') do
        local value=vim.trim(cell)
        row.cells[#row.cells+1]=value
        local rendered=M.inline(value)
        local n=#row.cells
        widths[n]=math.max(widths[n] or 0,vim.fn.strdisplaywidth(rendered))
      end
      row.text=raw
    else
      local heading=raw:match('^%s*#+%s+(.+)$')
      local quote=raw:match('^%s*>%s?(.*)$')
      local value=heading or quote or raw
      if not heading and not quote then value=value:gsub('^(%s*)[%-%*]%s+', '%1• ') end
      row.text,row.marks,row.links=M.inline(value)
      if heading then row.group='SosHeading'
      elseif quote then
        row.text='│ '..row.text; row.group='SosQuote'
        for _, mark in ipairs(row.marks) do mark.first=mark.first+#'│ '; mark.last=mark.last+#'│ ' end
        for _, link in ipairs(row.links) do link.first=link.first+#'│ '; link.last=link.last+#'│ ' end
      end
    end
    result[#result+1]=row
  end
  for _, row in ipairs(result) do
    if row.cells then
      row.text='│ '
      for n,cell in ipairs(row.cells) do
        local text,marks,links=M.inline(cell)
        local offset=#row.text
        row.text=row.text..text..string.rep(' ',math.max(0,(widths[n] or 0)-vim.fn.strdisplaywidth(text)))..' │ '
        for _,mark in ipairs(marks) do mark.first=mark.first+offset; mark.last=mark.last+offset; row.marks[#row.marks+1]=mark end
        for _,link in ipairs(links) do link.first=link.first+offset; link.last=link.last+offset; row.links[#row.links+1]=link end
      end
    end
  end
  return result
end
return M
