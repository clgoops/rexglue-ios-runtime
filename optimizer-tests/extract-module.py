from pathlib import Path
import sys
text=(Path(sys.argv[1])/'src/graphics/vulkan/shader.cpp').read_text(encoding='utf-8')
start=text.index('VkShaderModule VulkanShader::VulkanTranslation::GetOrCreateShaderModule()')
end=text.index('{',start)+1;depth=1
while depth:
 if text[end]=='{':depth+=1
 elif text[end]=='}':depth-=1
 end+=1
Path(sys.argv[2]).write_text(text[start:end]+'\n',encoding='utf-8',newline='\n')
