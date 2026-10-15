package org.d2r.binaryprobe;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.drawable.ColorDrawable;
import android.text.InputFilter;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Shared SDK text input; native controls own IME, focus and accessibility. */
final class ShellTextInputDialog {
    interface Result {void finish(boolean accepted,String text);}
    static AlertDialog show(Activity activity,String title,String sub,String hint,String initial,int min,int max,Result result) {
        int padding=(int)(18*activity.getResources().getDisplayMetrics().density+.5f);
        LinearLayout body=new LinearLayout(activity);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(padding,padding,padding,padding);
        if(!sub.isEmpty()){TextView description=new TextView(activity);description.setText(sub);description.setTextColor(TouchControlsView.TEXT);description.setTextSize(16);body.addView(description);}
        TextView label=new TextView(activity);label.setText(hint.isEmpty()?(title.isEmpty()?"输入内容":title):hint);label.setTextColor(TouchControlsView.TEXT);label.setTextSize(16);
        EditText input=new EditText(activity);input.setId(android.view.View.generateViewId());label.setLabelFor(input.getId());body.addView(label);
        input.setTextColor(TouchControlsView.TEXT);input.setTextSize(18);input.setSingleLine(true);input.setImeOptions(EditorInfo.IME_ACTION_DONE);
        input.setFilters(new InputFilter[]{new InputFilter.LengthFilter(max)});input.setText(initial);input.setSelection(input.length());body.addView(input,new LinearLayout.LayoutParams(-1,-2));
        ScrollView scroll=new ScrollView(activity);scroll.addView(body);boolean[] completed={false};
        AlertDialog dialog=new AlertDialog.Builder(activity).setTitle(title.isEmpty()?"输入内容":title).setView(scroll)
                .setPositiveButton("确认",null).setNegativeButton("取消",(d,w)->{if(!completed[0]){completed[0]=true;result.finish(false,"");}}).create();
        dialog.setCanceledOnTouchOutside(false);
        Runnable accept=()->{String text=input.getText().toString();if(text.length()<min){input.setError("至少输入 "+min+" 个字符");input.requestFocus();return;}
            if(!completed[0]){completed[0]=true;result.finish(true,text);dialog.dismiss();}};
        dialog.setOnShowListener(d->{dialog.getButton(AlertDialog.BUTTON_POSITIVE).setTextColor(TouchControlsView.TEXT);
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setTextColor(TouchControlsView.TEXT);dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->accept.run());
            input.requestFocus();dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE|WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);});
        input.setOnEditorActionListener((v,action,event)->{if(action==EditorInfo.IME_ACTION_DONE){accept.run();return true;}return false;});
        dialog.setOnDismissListener(d->{if(!completed[0]){completed[0]=true;result.finish(false,"");}});
        dialog.show();dialog.getWindow().setBackgroundDrawable(new ColorDrawable(0xff000000|TouchControlsView.INK));return dialog;
    }
}
