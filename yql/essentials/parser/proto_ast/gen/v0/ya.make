LIBRARY()

PEERDIR (
    yql/essentials/parser/proto_ast/gen/v0_proto_split
    yql/essentials/parser/proto_ast/antlr3
)

SET(antlr_output ${ARCADIA_BUILD_ROOT}/${MODDIR})
SET(antlr_templates ${antlr_output}/org/antlr/codegen/templates)
SET(sql_grammar ${ARCADIA_ROOT}/yql/essentials/sql/v0/SQL.g)

SET(ANTLR_PACKAGE_NAME NSQLGenerated)
SET(PROTOBUF_HEADER_PATH yql/essentials/parser/proto_ast/gen/v0_proto_split)
SET(PROTOBUF_SUFFIX_PATH .pb.main.h)
SET(LEXER_PARSER_NAMESPACE NALP)


CONFIGURE_FILE(${ARCADIA_ROOT}/yql/essentials/parser/proto_ast/org/antlr/codegen/templates/Cpp/Cpp.stg.in ${antlr_templates}/Cpp/Cpp.stg)

NO_COMPILER_WARNINGS()

INCLUDE(${ARCADIA_ROOT}/yql/essentials/parser/proto_ast/org/antlr/codegen/templates/ya.make.incl)

ADDINCL(
    GLOBAL contrib/libs/antlr4_cpp_runtime/src
)

RUN_ANTLR(
    ${sql_grammar}
    -lib .
    -fo ${antlr_output}
    IN ${sql_grammar} ${antlr_templates}/Cpp/Cpp.stg
    OUT SQLParser.cpp SQLLexer.cpp SQLParser.h SQLLexer.h
    OUTPUT_INCLUDES
    ${PROTOBUF_HEADER_PATH}/SQLParser.pb.main.h
    ${STG_INCLUDES}
    CWD ${antlr_output}
)

END()

