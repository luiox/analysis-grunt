import java.util.ArrayList;
import java.util.List;

/**
 * Grunt StringEncrypt 参照加密器（fixture 数据生成器，不进产品）。
 *
 * encrypt() 从 Grunt 源码逐行转写（grunt2 分支 36c0eef
 * grunt-main/src/main/kotlin/net/spartanb312/grunt/process/transformers/
 * encrypt/StringEncryptTransformer.kt 的 encrypt + createDecryptMethod，
 * grunt3 分支 e41e852 StringArrayedEncrypt.kt 逐字相同）。
 *
 * 用法：java GruntStringEncryptRef.java <plaintext> <seed> <key> <classKey>
 * 输出：cipher 的 UTF-16 码元十进制序列（逗号分隔），测试夹具按此嵌入
 * CASTORE / ldc 密文，fold 产物应还原为 <plaintext> 本身。
 */
public class GruntStringEncryptRef {

    /** Grunt 原函数（逐行转写，含 JVM int 溢出/移位屏蔽语义）。 */
    static String encrypt(char[] cArray, long seed, int key, int classKey) {
        int n = key;
        long l = seed;
        int n2 = classKey ^ n;

        for (int i = 0; i < cArray.length; i++) {
            n2 = n2 ^ (int) l ^ (~i);
            n2 = n2 ^ (n - i * cArray.length);
            n2 = (-n2 * n) | i;
            cArray[i] = (char) (cArray[i] ^ n2);
            int n3 = i & 0xFF;
            n = (n << n3) | (n >>> (-n3));
            l = l ^ n3;
        }
        return new String(cArray);
    }

    public static void main(String[] args) {
        String plaintext = args[0];
        long seed = Long.parseLong(args[1]);
        int key = Integer.parseInt(args[2]);
        int classKey = Integer.parseInt(args[3]);

        char[] chars = plaintext.toCharArray();
        String cipher = encrypt(chars, seed, key, classKey);

        List<String> units = new ArrayList<>();
        for (int i = 0; i < cipher.length(); i++) {
            units.add(Integer.toString(cipher.charAt(i)));
        }
        System.out.println("plain=" + plaintext);
        System.out.println("len=" + cipher.length());
        System.out.println("cipher=" + String.join(",", units));
        System.out.println("cipherEscaped=" + escape(cipher));
    }

    /** cipher 的 unicode 转义形（jasm ldc 用；反斜杠 u + 4 位十六进制）。 */
    static String escape(String s) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') {
                sb.append(c);
            } else {
                sb.append(String.format("\\u%04x", (int) c));
            }
        }
        return sb.toString();
    }
}
