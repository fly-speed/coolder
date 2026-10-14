package io.coolder.libai;

import java.util.*;

/** Private bounded codec. Raw contracts enter the native validator before decoding. */
final class TaskJson {
    private TaskJson() { }
    // Keep original number tokens. Do not turn 1e0 or 1.0 into integer version 1.
    private static final class NumberToken {
        final String wire; NumberToken(String wire){this.wire=wire;}
    }
    static String checked(String value) {
        if(value==null||value.indexOf('\0')>=0)throw new IllegalArgumentException("null/NUL task evidence");
        for(int i=0;i<value.length();++i){char c=value.charAt(i);
            if(Character.isHighSurrogate(c)){if(++i>=value.length()||!Character.isLowSurrogate(value.charAt(i)))throw new IllegalArgumentException("invalid task evidence Unicode");}
            else if(Character.isLowSurrogate(c))throw new IllegalArgumentException("invalid task evidence Unicode");
        }return value;
    }
    static Map<String,Object> object(Object... pairs){Map<String,Object> value=new LinkedHashMap<>();for(int i=0;i<pairs.length;i+=2)value.put((String)pairs[i],pairs[i+1]);return value;}
    static String write(Object value){StringBuilder out=new StringBuilder();append(out,value);return out.toString();}
    private static void append(StringBuilder out,Object value){
        if(value==null)out.append("null");
        else if(value instanceof String){out.append('"');String hex="0123456789abcdef";String text=(String)value;
            for(int i=0;i<text.length();++i){char c=text.charAt(i);if(c=='"'||c=='\\')out.append('\\').append(c);
                else if(c<32)out.append("\\u00").append(hex.charAt(c>>4)).append(hex.charAt(c&15));else out.append(c);}out.append('"');
        }else if(value instanceof NumberToken)out.append(((NumberToken)value).wire);
        else if(value instanceof Boolean||value instanceof Integer)out.append(value);
        else if(value instanceof Map){out.append('{');boolean first=true;for(Map.Entry<?,?> e:((Map<?,?>)value).entrySet()){
            if(!(e.getKey() instanceof String))throw new IllegalArgumentException("task JSON key must be string");
            if(!first)out.append(',');first=false;append(out,e.getKey());out.append(':');append(out,e.getValue());}out.append('}');
        }else if(value instanceof List){out.append('[');boolean first=true;for(Object item:(List<?>)value){if(!first)out.append(',');first=false;append(out,item);}out.append(']');}
        else throw new IllegalArgumentException("unsupported task JSON value");
    }
    static Object read(String wire,int maxChars){Parser p=new Parser(wire,maxChars);Object value=p.value(0);p.space();if(p.at!=wire.length())throw p.bad();return value;}
    @SuppressWarnings("unchecked") static Map<String,Object> map(Object value){if(!(value instanceof Map))throw new IllegalArgumentException("task JSON object expected");return (Map<String,Object>)value;}
    static String text(Map<String,Object> value,String key,String fallback){if(!value.containsKey(key))return fallback;Object item=value.get(key);if(!(item instanceof String))throw new IllegalArgumentException("task string expected");return checked((String)item);}
    static boolean bool(Map<String,Object> value,String key,boolean fallback){if(!value.containsKey(key))return fallback;Object item=value.get(key);if(!(item instanceof Boolean))throw new IllegalArgumentException("task boolean expected");return (Boolean)item;}
    private static final class Parser {
        final String wire;int at,nodes;
        Parser(String wire,int limit){if(wire==null||wire.length()>limit)throw new IllegalArgumentException("task JSON size limit");this.wire=wire;}
        IllegalArgumentException bad(){return new IllegalArgumentException("invalid/ambiguous task JSON");}
        void space(){while(at<wire.length()&&" \t\r\n".indexOf(wire.charAt(at))>=0)++at;}
        char take(){if(at==wire.length())throw bad();return wire.charAt(at++);}
        void expected(char c){space();if(take()!=c)throw bad();}
        Object value(int depth){space();if(depth>64||++nodes>65536||at==wire.length())throw bad();char c=wire.charAt(at);
            if(depth==64&&(c=='{'||c=='['))throw bad();
            if(c=='"')return string();
            if(c=='{'){++at;Map<String,Object> out=new LinkedHashMap<>();space();if(at<wire.length()&&wire.charAt(at)=='}'){++at;return out;}
                for(;;){space();if(at==wire.length()||wire.charAt(at)!='"')throw bad();String key=string();if(out.containsKey(key))throw bad();expected(':');out.put(key,value(depth+1));space();char end=take();if(end=='}')return out;if(end!=',')throw bad();}}
            if(c=='['){++at;List<Object> out=new ArrayList<>();space();if(at<wire.length()&&wire.charAt(at)==']'){++at;return out;}
                for(;;){out.add(value(depth+1));space();char end=take();if(end==']')return out;if(end!=',')throw bad();}}
            if(wire.startsWith("true",at)){at+=4;return Boolean.TRUE;}if(wire.startsWith("false",at)){at+=5;return Boolean.FALSE;}if(wire.startsWith("null",at)){at+=4;return null;}
            int start=at;if(c=='-')++at;if(at==wire.length())throw bad();
            if(wire.charAt(at)=='0')++at;else{int digit=at;while(at<wire.length()&&wire.charAt(at)>='0'&&wire.charAt(at)<='9')++at;if(at==digit)throw bad();}
            
            if(at<wire.length()&&wire.charAt(at)=='.'){++at;int digit=at;while(at<wire.length()&&wire.charAt(at)>='0'&&wire.charAt(at)<='9')++at;if(at==digit)throw bad();}
            if(at<wire.length()&&(wire.charAt(at)=='e'||wire.charAt(at)=='E')){++at;if(at<wire.length()&&(wire.charAt(at)=='+'||wire.charAt(at)=='-'))++at;int digit=at;while(at<wire.length()&&wire.charAt(at)>='0'&&wire.charAt(at)<='9')++at;if(at==digit)throw bad();}
            return new NumberToken(wire.substring(start,at));
        }
        String string(){expected('"');StringBuilder out=new StringBuilder();for(;;){char c=take();if(c=='"'){
                String result=out.toString();for(int i=0;i<result.length();++i){char u=result.charAt(i);
                    if(Character.isHighSurrogate(u)){if(++i>=result.length()||!Character.isLowSurrogate(result.charAt(i)))throw bad();}
                    else if(Character.isLowSurrogate(u))throw bad();}return result;
            }if(c<32)throw bad();
            if(c!='\\'){out.append(c);continue;}c=take();switch(c){case '"':case '\\':case '/':out.append(c);break;case 'b':out.append('\b');break;case 'f':out.append('\f');break;case 'n':out.append('\n');break;case 'r':out.append('\r');break;case 't':out.append('\t');break;
                case 'u':int code=0;for(int i=0;i<4;++i){char h=take();int d="0123456789abcdef".indexOf(h);if(d<0)d="0123456789ABCDEF".indexOf(h);if(d<0)throw bad();code=code*16+d;}out.append((char)code);break;default:throw bad();}}
        }
    }
}
