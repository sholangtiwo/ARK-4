#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H

#include "common.h"
#include "entry.h"
#include "system_entry.h"

typedef struct {
    char* description;
    unsigned char max_options;
    unsigned char selection;
    unsigned char* config_ptr;
    char* options[];
} settings_entry;

typedef struct {
    settings_entry** settings_entries;
    int max_options;
} SettingsTable;

class SettingsMenu : public SystemEntry{

    private:
    
        int animation;
        int w, h, x, y;
        int index, start;
        
        string* customText;
        int ntext;
        
        bool changed;
        
        SettingsTable* table;
        int max_height;
        
        string info;
        string name;
        
        Image* icon;

        void (*callback)();
        
    public:
    
        SettingsMenu(SettingsTable* table, void (*callback)());
        ~SettingsMenu();
    
        void setCustomText(string text[], int n);
        void unsetCustomText();
    
        void setIcon(Image* icon){
            if (icon) this->icon = icon;
        }

        void draw();
        
        void control(Controller* pad);
        
        void pause();
        
        void resume();
        
        void setInfo(string info){
            this->info = info;
        }
        
        void setName(string name){
            this->name = name;
        }
        
        string getInfo(){
            return this->info;
        }
        
        Image* getIcon(){
            return this->icon;
        }
        
        string getName(){
            return this->name;
        }

        bool isStillLoading(){ return false; }
        
        void applyConf();
        void readConf();
        
};

#endif
