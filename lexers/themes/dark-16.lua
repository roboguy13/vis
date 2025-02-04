-- Eight-color scheme
local lexers = vis.lexers
-- dark
lexers.STYLE_DEFAULT = 'back:black,fore:white'
lexers.STYLE_NOTHING = 'back:black'
lexers.STYLE_CLASS = 'fore:yellow'
lexers.STYLE_COMMENT = 'fore:blue'
lexers.STYLE_CONSTANT = 'fore:cyan'
lexers.STYLE_DEFINITION = 'fore:blue'
lexers.STYLE_ERROR = 'fore:red,italics'
lexers.STYLE_FUNCTION = 'fore:blue,bold'
lexers.STYLE_KEYWORD = 'fore:yellow'
lexers.STYLE_LABEL = 'fore:green'
lexers.STYLE_NUMBER = 'fore:red'
lexers.STYLE_OPERATOR = 'fore:cyan'
lexers.STYLE_REGEX = 'fore:green'
lexers.STYLE_STRING = 'fore:red'
lexers.STYLE_PREPROCESSOR = 'fore:magenta'
lexers.STYLE_TAG = 'fore:red'
lexers.STYLE_TYPE = 'fore:green'
lexers.STYLE_VARIABLE = 'fore:blue,bold'
lexers.STYLE_WHITESPACE = ''
lexers.STYLE_EMBEDDED = 'back:blue'
lexers.STYLE_IDENTIFIER = 'fore:white'

lexers.STYLE_LINENUMBER = 'fore:white'
lexers.STYLE_CURSOR = 'reverse'
lexers.STYLE_CURSOR_LINE = 'back:white'
lexers.STYLE_COLOR_COLUMN = 'back:white'
lexers.STYLE_SELECTION = 'back:white'
