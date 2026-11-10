package dev.hyperglass.lsposed;
import android.app.Activity;
import android.content.ComponentName;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
public final class MainActivity extends Activity {
 private ComponentName launcherAlias;
 private Button toggle;
 @Override public void onCreate(Bundle state) {
  super.onCreate(state);
  launcherAlias=new ComponentName(this,getPackageName()+".Launcher");
  int pad=(int)(24*getResources().getDisplayMetrics().density);
  LinearLayout layout=new LinearLayout(this);
  layout.setOrientation(LinearLayout.VERTICAL);
  layout.setPadding(pad,pad,pad,pad);
  TextView text=new TextView(this);
  text.setTextSize(18);
  text.setText("HyperGlass 1.0.4\n\n在 LSPosed / Vector 中启用本模块，作用域选择系统桌面（com.miui.home），然后重启系统桌面。\n\n包含单色玻璃图标、动态日历和时钟、文件夹、MAML 及普通 Android 小组件。\n\n需要支持 HyOS 原生进程的框架和 Modern API 102。自动读取当前桌面及渲染库，通过方法符号、ARM64 语义与调用关系定位。类编号、关键字段和栈槽随当前二进制推导。遇到缺失或多义匹配时停用相应功能并记录原因。\n\n模块启停后需重启桌面；卸载模块后重启桌面即可恢复。运行诊断见日志标签 HyperGlass。");
  layout.addView(text);
  toggle=new Button(this);
  toggle.setOnClickListener(view->setLauncherHidden(!isLauncherHidden()));
  layout.addView(toggle,new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,ViewGroup.LayoutParams.WRAP_CONTENT));
  TextView hint=new TextView(this);
  hint.setTextSize(14);
  hint.setText("隐藏桌面图标后，从 LSPosed / Vector 的模块页面重新打开本页即可恢复。");
  layout.addView(hint);
  setContentView(layout);
  updateToggle();
 }
 private boolean isLauncherHidden() {
  int state=getPackageManager().getComponentEnabledSetting(launcherAlias);
  return state==PackageManager.COMPONENT_ENABLED_STATE_DISABLED||state==PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER||state==PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED;
 }
 private void setLauncherHidden(boolean hidden) {
  getPackageManager().setComponentEnabledSetting(launcherAlias,hidden?PackageManager.COMPONENT_ENABLED_STATE_DISABLED:PackageManager.COMPONENT_ENABLED_STATE_ENABLED,PackageManager.DONT_KILL_APP);
  updateToggle();
 }
 private void updateToggle() {
  toggle.setText(isLauncherHidden()?"显示桌面图标":"隐藏桌面图标");
 }
}
