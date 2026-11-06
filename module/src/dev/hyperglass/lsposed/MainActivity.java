package dev.hyperglass.lsposed;
import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
public final class MainActivity extends Activity {
 @Override public void onCreate(Bundle state) {
  super.onCreate(state);
  TextView text=new TextView(this);
  text.setTextSize(18);
  int pad=(int)(24*getResources().getDisplayMetrics().density);
  text.setPadding(pad,pad,pad,pad);
  text.setText("HyperGlass 1.0.2\n\n在 LSPosed / Vector 中启用本模块，作用域选择系统桌面（com.miui.home），然后重启系统桌面。\n\n包含单色玻璃图标、动态日历和时钟、文件夹、MAML 及普通 Android 小组件。\n\n需要支持 HyOS 原生进程的框架和 Modern API 102。自动读取当前桌面及渲染库，通过方法符号、ARM64 语义与调用关系定位。类编号、关键字段和栈槽随当前二进制推导。遇到缺失或多义匹配时停用相应功能并记录原因。\n\n模块启停后需重启桌面；卸载模块后重启桌面即可恢复。运行诊断见日志标签 HyperGlass。");
  setContentView(text);
 }
}
