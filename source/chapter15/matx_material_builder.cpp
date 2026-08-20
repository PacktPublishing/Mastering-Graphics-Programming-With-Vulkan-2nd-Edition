#include "MaterialXGenSlang/SlangShaderGenerator.h"
#include "MaterialXCore/Document.h"
#include "MaterialXFormat/XmlIo.h"
#include "MaterialXFormat/File.h"
#include "MaterialXFormat/Util.h"

#include <stdio.h>
#include <fstream>

namespace mx = MaterialX;

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <path_to_mtlx_file>\n", argv[0]);
        return -1;
    }

    const char* file_path = argv[1];

    mx::GenContext context(mx::SlangShaderGenerator::create());

    mx::FileSearchPath searchPath("D:/workspace/MaterialX/_out");
    context.registerSourceCodeSearchPath(searchPath);

    mx::DocumentPtr libraries = mx::createDocument();
    mx::loadLibraries({ "libraries" }, searchPath, libraries);

    mx::DocumentPtr testDoc = mx::createDocument();
    mx::readFromXmlFile(testDoc, mx::FilePath(file_path));
    testDoc->setDataLibrary(libraries);

    mx::string materialNode = "Car_Paint";
    mx::ElementPtr element = testDoc->getChild(materialNode);

    mx::ShaderPtr shader = context.getShaderGenerator().generate(materialNode, element, context);

    auto& blocks = shader->getStage(1).getUniformBlocks();
    for (const auto& block : blocks) {
        printf("Uniform Block: %s\n", block.first.c_str());
        auto blockPtr = block.second;
        size_t numVars = blockPtr->size();
        for (size_t i = 0; i < numVars; ++i) {
            auto var = (*blockPtr)[i];
            auto value = var->getValue();
            if (value) {
                printf("  Variable: %s, Type: %s, Value: %s\n", var->getName().c_str(), var->getType().getName().c_str(), value->getValueString().c_str());
            } else {
                printf("  Variable: %s, Type: %s\n", var->getName().c_str(), var->getType().getName().c_str());
            }
        }

    }

    const std::string& shaderCode = shader->getSourceCode();

    std::ofstream out("shader.slang");
    out << shaderCode;
    out.close();

    return 0;
}
