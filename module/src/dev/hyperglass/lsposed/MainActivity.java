package dev.hyperglass.lsposed;
import android.app.Activity;
import android.content.ComponentName;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.ScrollView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
public final class MainActivity extends Activity {
 private ComponentName launcherAlias;
 private Button toggle,monochrome,badges;
 private File monochromeSetting,badgeSetting;
 @Override public void onCreate(Bundle state) {
  super.onCreate(state);
  launcherAlias=new ComponentName(this,getPackageName()+".Launcher");
  int pad=(int)(24*getResources().getDisplayMetrics().density);
  LinearLayout layout=new LinearLayout(this);
  layout.setOrientation(LinearLayout.VERTICAL);
  layout.setPadding(pad,pad,pad,pad);
  TextView text=new TextView(this);
  text.setTextSize(18);
  text.setText("HyperGlass 1.0.6\n\n在 LSPosed / Vector 中启用本模块，作用域选择系统桌面（com.miui.home），然后重启系统桌面。\n\n包含单色玻璃图标、动态日历和时钟、文件夹、MAML 及普通 Android 小组件。\n\n需要支持 HyOS 原生进程的框架和 Modern API 102。自动读取当前桌面及渲染库，通过方法符号、ARM64 语义与调用关系定位。类编号、关键字段和栈槽随当前二进制推导。遇到缺失或多义匹配时停用相应功能并记录原因。\n\n模块启停后需重启桌面；卸载模块后重启桌面即可恢复。运行诊断见日志标签 HyperGlass。");
  layout.addView(text);
  toggle=new Button(this);
  toggle.setOnClickListener(view->setLauncherHidden(!isLauncherHidden()));
  layout.addView(toggle,new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,ViewGroup.LayoutParams.WRAP_CONTENT));
  TextView hint=new TextView(this);
  hint.setTextSize(14);
  hint.setText("隐藏桌面图标后，从 LSPosed / Vector 的模块页面重新打开本页即可恢复。");
  layout.addView(hint);
  monochromeSetting=new File(getExternalMediaDirs()[0],"widget-monochrome");
  monochrome=new Button(this);
  monochrome.setOnClickListener(view->setMonochrome(!isMonochrome()));
  layout.addView(monochrome,new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,ViewGroup.LayoutParams.WRAP_CONTENT));
  TextView monochromeHint=new TextView(this);
  monochromeHint.setTextSize(14);
  monochromeHint.setText("开启时小组件内的图标、图片和照片以单色显示；关闭后保留原色，文字仍为白色。修改后重启系统桌面生效。");
  layout.addView(monochromeHint);
  badgeSetting=new File(getExternalMediaDirs()[0],"badge-monochrome");
  badges=new Button(this);
  badges.setOnClickListener(view->setBadgeMonochrome(!isBadgeMonochrome()));
  layout.addView(badges,new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,ViewGroup.LayoutParams.WRAP_CONTENT));
  TextView badgeHint=new TextView(this);
  badgeHint.setTextSize(14);
  badgeHint.setText("开启后通知角标显示为白底黑字；关闭后使用系统配色。修改后重启系统桌面生效。");
  layout.addView(badgeHint);
  ScrollView scroll=new ScrollView(this);
  scroll.addView(layout);
  setContentView(scroll);
  updateToggle();
  updateMonochrome();
  updateBadges();
 }
 private boolean isBadgeMonochrome() {
  try {
   return "1".equals(new String(Files.readAllBytes(badgeSetting.toPath()),StandardCharsets.US_ASCII).trim());
  } catch (IOException e) {
   return false;
  }
 }
 private void setBadgeMonochrome(boolean enabled) {
  try (FileOutputStream out=new FileOutputStream(badgeSetting)) {
   out.write(enabled?'1':'0');
  } catch (IOException e) {
   badges.setText("无法写入设置："+e.getMessage());
   return;
  }
  updateBadges();
 }
 private void updateBadges() {
  badges.setText(isBadgeMonochrome()?"通知角标白底黑字：已开启":"通知角标白底黑字：已关闭");
 }
 private boolean isMonochrome() {
  try {
   return !"0".equals(new String(Files.readAllBytes(monochromeSetting.toPath()),StandardCharsets.US_ASCII).trim());
  } catch (IOException e) {
   return true;
  }
 }
 private void setMonochrome(boolean enabled) {
  try (FileOutputStream out=new FileOutputStream(monochromeSetting)) {
   out.write(enabled?'1':'0');
  } catch (IOException e) {
   monochrome.setText("无法写入设置："+e.getMessage());
   return;
  }
  updateMonochrome();
 }
 private void updateMonochrome() {
  monochrome.setText(isMonochrome()?"小组件单色：已开启":"小组件单色：已关闭");
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
