#pragma once
#include "../base/base.h"
#include "../radio/radiolink.h"
#include "../radio/radiopacketsqueue.h"

#define PACKET_TYPE_LOCAL_CONTROL_RENDERER_REQUEST_BODY_LENGTH 0x200

// TODO Hope to get rid of this soon.. 
typedef enum 
{
    frame_start,
    frame_end,
    stroke_set,
    stroke_set_size,
    raw_font_load,
    raw_font_free,
    raw_font_color_outline,
    image_load,
    image_free,
    image_draw,
    image_draw_alpha,
    image_blt,
    image_blt_sprite,
    icon_load,
    icon_free,
    icon_draw,
    icon_blt,
    hue_set,
    rotate,
    line_draw_h,
    line_draw_v,
    line_draw,
    round_rect_draw,
    round_rect_draw_menu,
    triangle_draw,
    triangle_fill,
    circle_draw,
    circle_fill,
    polyline_draw,
    polyline_fill,
    arc_draw,
    text_draw,
    text_draw_scaled,
    
} t_packet_renderer_request_type;

typedef struct
{
    t_packet_renderer_request_type type;
    u8 body[PACKET_TYPE_LOCAL_CONTROL_RENDERER_REQUEST_BODY_LENGTH-1];

    float fValue(u32 offset) 
    {
         float f;
         memcpy(&f, body+offset, sizeof(float));
         return f;
    }

    char* strValue(u32 offset) 
    {
        // int strLen = -1, i = 0;
        
        // while(i++ < 256) 
        // {
        //     if ( body[offset+i] == '\0' )
        //     {
        //         strLen = i;
        //         break;
        //     }
        // }

        return (char*)(body + offset);
    }

    void* ptrValue(u32 offset) 
    {
        return (void*)(body + offset);
    }

    int intValue(u32 offset) 
    {
        int i;
        memcpy(&i, body+offset, sizeof(int));
        return i;
    }

    u8  u8Value(u32 offset) 
    {
        u8 val;
        memcpy(&val, body+offset, sizeof(u8));
        return val;
    }

    u16 u16Value(u32 offset) 
    {
        u16 val;
        memcpy(&val, body+offset, sizeof(u16));
        return val; 
    }

    u32 u32Value(u32 offset) 
    {
        u32 val;
        memcpy(&val, body+offset, sizeof(u32));
        return val; 
    }
} __attribute__((packed)) t_packet_renderer_request_data;

void process_local_control_packet(u8* pPacketBuffer);
