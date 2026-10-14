package io.coolder.libai;
import java.util.*;

/** Private value codec. Saved index JSON bypasses it and enters native validation unchanged. */
final class RagJson {
    private RagJson() { }
    static String checked(String value) {
        if(value==null||value.indexOf('\0')>=0)throw new IllegalArgumentException("null/NUL RAG text");
        for(int i=0;i<value.length();++i){char c=value.charAt(i);
            if(Character.isHighSurrogate(c)){if(++i>=value.length()||!Character.isLowSurrogate(value.charAt(i)))throw new IllegalArgumentException("invalid RAG Unicode");}
            else if(Character.isLowSurrogate(c))throw new IllegalArgumentException("invalid RAG Unicode");
        }return value;
    }
    static Map<String,Object> object(Object... pairs) {
        Map<String,Object> value=new LinkedHashMap<>();
        for(int i=0;i<pairs.length;i+=2)value.put((String)pairs[i],pairs[i+1]);
        return value;
    }
    static String write(Object value) {StringBuilder out=new StringBuilder();append(out,value);return out.toString();}
    private static void append(StringBuilder out,Object value) {
        if(value instanceof String){String text=checked((String)value);out.append('"');String hex="0123456789abcdef";
            for(int i=0;i<text.length();++i){char c=text.charAt(i);
                if(c=='"'||c=='\\')out.append('\\').append(c);
                else if(c<32)out.append("\\u00").append(hex.charAt(c>>4)).append(hex.charAt(c&15));else out.append(c);
            }out.append('"');
        }else if(value instanceof Boolean)out.append(value);
        else if(value instanceof Number){double n=((Number)value).doubleValue();if(Double.isNaN(n)||Double.isInfinite(n))throw new IllegalArgumentException("nonfinite RAG number");out.append(value);}
        else if(value instanceof Map){out.append('{');boolean first=true;for(Map.Entry<?,?> e:((Map<?,?>)value).entrySet()){if(!first)out.append(',');first=false;append(out,e.getKey());out.append(':');append(out,e.getValue());}out.append('}');}
        else if(value instanceof Object[]){out.append('[');Object[] items=(Object[])value;for(int i=0;i<items.length;++i){if(i>0)out.append(',');append(out,items[i]);}out.append(']');}
        else if(value instanceof double[]){double[] items=(double[])value;out.append('[');for(int i=0;i<items.length;++i){if(i>0)out.append(',');append(out,items[i]);}out.append(']');}
        else throw new IllegalArgumentException("unsupported RAG JSON value");
    }
    static Object read(String wire){Parser parser=new Parser(wire);Object value=parser.value(0);parser.space();if(parser.at!=wire.length())throw new IllegalStateException("trailing native RAG JSON");return value;}
    @SuppressWarnings("unchecked") static Map<String,Object> map(Object value){if(!(value instanceof Map))throw new IllegalStateException("RAG JSON object expected");return (Map<String,Object>)value;}
    @SuppressWarnings("unchecked") static List<Object> list(Object value){if(!(value instanceof List))throw new IllegalStateException("RAG JSON array expected");return (List<Object>)value;}
    static String text(Map<String,Object> value,String key){Object item=value.get(key);if(!(item instanceof String))throw new IllegalStateException("RAG string expected");return (String)item;}
    static long integer(Map<String,Object> value,String key){Object item=value.get(key);if(!(item instanceof Long))throw new IllegalStateException("RAG integer expected");return (Long)item;}
    static double number(Map<String,Object> value,String key){Object item=value.get(key);if(!(item instanceof Number))throw new IllegalStateException("RAG number expected");return ((Number)item).doubleValue();}
    static boolean bool(Map<String,Object> value,String key){Object item=value.get(key);if(!(item instanceof Boolean))throw new IllegalStateException("RAG boolean expected");return (Boolean)item;}
    private static final class Parser {
        final String wire;int at,nodes;
        Parser(String wire){this.wire=wire;if(wire==null||wire.length()>128*1024*1024)throw new IllegalStateException("RAG wire limit");}
        void space(){while(at<wire.length()&&" \t\r\n".indexOf(wire.charAt(at))>=0)++at;}
        IllegalStateException bad(){return new IllegalStateException("invalid native RAG JSON");}
        char take(){if(at==wire.length())throw bad();return wire.charAt(at++);}
        void expected(char c){space();if(take()!=c)throw bad();}
        Object value(int depth){space();if(depth>=64||++nodes>5*1024*1024||at==wire.length())throw bad();char c=wire.charAt(at);
            if(c=='"')return string();
            if(c=='{'){++at;Map<String,Object> out=new LinkedHashMap<>();space();if(at<wire.length()&&wire.charAt(at)=='}'){++at;return out;}for(;;){space();if(at==wire.length()||wire.charAt(at)!='"')throw bad();String key=string();if(out.containsKey(key))throw bad();expected(':');out.put(key,value(depth+1));space();char end=take();if(end=='}')return out;if(end!=',')throw bad();}}
            if(c=='['){++at;List<Object> out=new ArrayList<>();space();if(at<wire.length()&&wire.charAt(at)==']'){++at;return out;}for(;;){out.add(value(depth+1));space();char end=take();if(end==']')return out;if(end!=',')throw bad();}}
            if(wire.startsWith("true",at)){at+=4;return Boolean.TRUE;}if(wire.startsWith("false",at)){at+=5;return Boolean.FALSE;}
            int start=at;if(c=='-')++at;if(at==wire.length())throw bad();
            if(wire.charAt(at)=='0')++at;else{int digit=at;while(at<wire.length()&&wire.charAt(at)>='0'&&wire.charAt(at)<='9')++at;if(at==digit)throw bad();}
            boolean floating=false;
            if(at<wire.length()&&wire.charAt(at)=='.'){floating=true;++at;int digit=at;while(at<wire.length()&&wire.charAt(at)>='0'&&wire.charAt(at)<='9')++at;if(at==digit)throw bad();}
            if(at<wire.length()&&(wire.charAt(at)=='e'||wire.charAt(at)=='E')){floating=true;++at;if(at<wire.length()&&(wire.charAt(at)=='+'||wire.charAt(at)=='-'))++at;int digit=at;while(at<wire.length()&&wire.charAt(at)>='0'&&wire.charAt(at)<='9')++at;if(at==digit)throw bad();}
            String text=wire.substring(start,at);try{if(!floating)return Long.valueOf(text);double n=Double.parseDouble(text);if(Double.isInfinite(n)||Double.isNaN(n))throw bad();return n;}catch(NumberFormatException error){throw bad();}
        }
        String string(){if(take()!='"')throw bad();StringBuilder out=new StringBuilder();for(;;){char c=take();if(c=='"')return checked(out.toString());if(c<32)throw bad();
            if(c!='\\'){out.append(c);continue;}c=take();switch(c){case '"':case '\\':case '/':out.append(c);break;case 'b':out.append('\b');break;case 'f':out.append('\f');break;case 'n':out.append('\n');break;case 'r':out.append('\r');break;case 't':out.append('\t');break;case 'u':int code=0;for(int i=0;i<4;++i){int digit=Character.digit(take(),16);if(digit<0)throw bad();code=code*16+digit;}out.append((char)code);break;default:throw bad();}
        }}
    }
}
