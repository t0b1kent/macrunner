#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

struct V { float p[4]; float c[4]; float uv[2]; };

static uint8_t clamp_channel(double v) {
    if (v <= 1.0) v *= 255.0;
    if (v < 0.0) return 0;
    if (v > 255.0) return 255;
    return (uint8_t)(v + 0.5);
}

static float clamp_float01(double v) {
    if (v > 1.0) v /= 255.0;
    if (v < 0.0) return 0.0f;
    if (v > 1.0) return 1.0f;
    return (float)v;
}

static float matrix_value(NSArray *matrix, NSUInteger index, float fallback) {
    if (![matrix isKindOfClass:[NSArray class]] || [matrix count] <= index) return fallback;
    return [[matrix objectAtIndex:index] floatValue];
}

static void apply_wvp(float p[4], NSArray *matrix) {
    if (![matrix isKindOfClass:[NSArray class]] || [matrix count] != 16) return;
    float in[4] = { p[0], p[1], p[2], p[3] };
    for (NSUInteger col = 0; col < 4; col++) {
        p[col] =
            in[0] * matrix_value(matrix, col, 0.0f) +
            in[1] * matrix_value(matrix, 4 + col, 0.0f) +
            in[2] * matrix_value(matrix, 8 + col, 0.0f) +
            in[3] * matrix_value(matrix, 12 + col, 0.0f);
    }
}

static NSMutableData *vertex_data_from_request(NSArray *vertices, NSArray *wvp) {
    NSMutableData *data = [NSMutableData data];
    if ([vertices isKindOfClass:[NSArray class]] && [vertices count] > 0) {
        for (id item in vertices) {
            if (![item isKindOfClass:[NSDictionary class]]) continue;
            NSDictionary *vertex = item;
            NSArray *position = vertex[@"position"];
            NSArray *color = vertex[@"color"];
            NSArray *uv = vertex[@"uv"];
            if (![position isKindOfClass:[NSArray class]] || [position count] < 2) continue;
            float p[4] = {
                [[position objectAtIndex:0] floatValue],
                [[position objectAtIndex:1] floatValue],
                [position count] > 2 ? [[position objectAtIndex:2] floatValue] : 0.0f,
                [position count] > 3 ? [[position objectAtIndex:3] floatValue] : 1.0f,
            };
            apply_wvp(p, wvp);
            struct V v = {
                { p[0], p[1], p[2], p[3] },
                {
                    [color isKindOfClass:[NSArray class]] && [color count] > 0 ? clamp_float01([[color objectAtIndex:0] doubleValue]) : 1.0f,
                    [color isKindOfClass:[NSArray class]] && [color count] > 1 ? clamp_float01([[color objectAtIndex:1] doubleValue]) : 1.0f,
                    [color isKindOfClass:[NSArray class]] && [color count] > 2 ? clamp_float01([[color objectAtIndex:2] doubleValue]) : 1.0f,
                    [color isKindOfClass:[NSArray class]] && [color count] > 3 ? clamp_float01([[color objectAtIndex:3] doubleValue]) : 1.0f,
                },
                {
                    [uv isKindOfClass:[NSArray class]] && [uv count] > 0 ? [[uv objectAtIndex:0] floatValue] : 0.0f,
                    [uv isKindOfClass:[NSArray class]] && [uv count] > 1 ? [[uv objectAtIndex:1] floatValue] : 0.0f,
                },
            };
            [data appendBytes:&v length:sizeof(v)];
        }
    }
    if ([data length] == 0) {
        struct V fallback[3] = {
            {{ 0.0f,  0.75f, 0.0f, 1.0f}, {1, 0, 0, 1}, {0.5f, 0.0f}},
            {{-0.75f, -0.75f, 0.0f, 1.0f}, {0, 1, 0, 1}, {0.0f, 1.0f}},
            {{ 0.75f, -0.75f, 0.0f, 1.0f}, {0, 0, 1, 1}, {1.0f, 1.0f}},
        };
        [data appendBytes:fallback length:sizeof(fallback)];
    }
    return data;
}

static NSMutableData *index_data_from_request(NSArray *indices, BOOL index32) {
    NSMutableData *data = [NSMutableData data];
    if (![indices isKindOfClass:[NSArray class]]) return data;
    for (id item in indices) {
        if (index32) {
            uint32_t index = (uint32_t)[item unsignedIntegerValue];
            [data appendBytes:&index length:sizeof(index)];
        } else {
            uint16_t index = (uint16_t)[item unsignedIntegerValue];
            [data appendBytes:&index length:sizeof(index)];
        }
    }
    return data;
}

static NSMutableData *texture_data_from_request(NSArray *pixels) {
    NSMutableData *data = [NSMutableData data];
    if ([pixels isKindOfClass:[NSArray class]] && [pixels count] > 0) {
        for (id item in pixels) {
            if (![item isKindOfClass:[NSArray class]]) continue;
            NSArray *pixel = item;
            uint8_t rgba[4] = {
                [pixel count] > 0 ? clamp_channel([[pixel objectAtIndex:0] doubleValue]) : 255,
                [pixel count] > 1 ? clamp_channel([[pixel objectAtIndex:1] doubleValue]) : 255,
                [pixel count] > 2 ? clamp_channel([[pixel objectAtIndex:2] doubleValue]) : 255,
                [pixel count] > 3 ? clamp_channel([[pixel objectAtIndex:3] doubleValue]) : 255,
            };
            [data appendBytes:rgba length:sizeof(rgba)];
        }
    }
    if ([data length] == 0) {
        uint8_t fallback[16] = {
            255, 255,   0, 255,
              0, 255, 255, 255,
            255,   0, 255, 255,
            255, 255, 255, 255,
        };
        [data appendBytes:fallback length:sizeof(fallback)];
    }
    return data;
}

static MTLBlendFactor blend_factor_from_d3d9(NSString *blend) {
    if ([blend isEqualToString:@"D3DBLEND_ZERO"]) return MTLBlendFactorZero;
    if ([blend isEqualToString:@"D3DBLEND_SRCALPHA"]) return MTLBlendFactorSourceAlpha;
    if ([blend isEqualToString:@"D3DBLEND_INVSRCALPHA"]) return MTLBlendFactorOneMinusSourceAlpha;
    if ([blend isEqualToString:@"D3DBLEND_DESTALPHA"]) return MTLBlendFactorDestinationAlpha;
    if ([blend isEqualToString:@"D3DBLEND_INVDESTALPHA"]) return MTLBlendFactorOneMinusDestinationAlpha;
    if ([blend isEqualToString:@"D3DBLEND_SRCCOLOR"]) return MTLBlendFactorSourceColor;
    if ([blend isEqualToString:@"D3DBLEND_INVSRCCOLOR"]) return MTLBlendFactorOneMinusSourceColor;
    if ([blend isEqualToString:@"D3DBLEND_DESTCOLOR"]) return MTLBlendFactorDestinationColor;
    if ([blend isEqualToString:@"D3DBLEND_INVDESTCOLOR"]) return MTLBlendFactorOneMinusDestinationColor;
    return MTLBlendFactorOne;
}

static MTLColorWriteMask color_write_mask_from_d3d9(id value) {
    if (!value || value == (id)[NSNull null]) return MTLColorWriteMaskAll;
    NSUInteger bits = 0x0f;
    if ([value isKindOfClass:[NSString class]]) {
        NSString *text = [(NSString *)value stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if ([text hasPrefix:@"0x"] || [text rangeOfCharacterFromSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]].location == NSNotFound) {
            bits = strtoul([text UTF8String], NULL, 0);
        } else {
            bits = 0;
            if ([text containsString:@"RED"]) bits |= 0x1;
            if ([text containsString:@"GREEN"]) bits |= 0x2;
            if ([text containsString:@"BLUE"]) bits |= 0x4;
            if ([text containsString:@"ALPHA"]) bits |= 0x8;
        }
    } else if ([value respondsToSelector:@selector(unsignedIntegerValue)]) {
        bits = [value unsignedIntegerValue];
    }
    MTLColorWriteMask mask = 0;
    if (bits & 0x1) mask |= MTLColorWriteMaskRed;
    if (bits & 0x2) mask |= MTLColorWriteMaskGreen;
    if (bits & 0x4) mask |= MTLColorWriteMaskBlue;
    if (bits & 0x8) mask |= MTLColorWriteMaskAlpha;
    return mask;
}

static MTLSamplerAddressMode address_mode_from_d3d9(NSString *mode) {
    if ([mode isEqualToString:@"D3DTADDRESS_WRAP"]) return MTLSamplerAddressModeRepeat;
    if ([mode isEqualToString:@"D3DTADDRESS_MIRROR"]) return MTLSamplerAddressModeMirrorRepeat;
    return MTLSamplerAddressModeClampToEdge;
}

static MTLSamplerMinMagFilter filter_from_d3d9(NSString *filter) {
    if ([filter isEqualToString:@"D3DTEXF_LINEAR"]) return MTLSamplerMinMagFilterLinear;
    return MTLSamplerMinMagFilterNearest;
}

static MTLCompareFunction compare_function_from_d3d9(NSString *func) {
    if ([func isEqualToString:@"D3DCMP_NEVER"]) return MTLCompareFunctionNever;
    if ([func isEqualToString:@"D3DCMP_LESS"]) return MTLCompareFunctionLess;
    if ([func isEqualToString:@"D3DCMP_EQUAL"]) return MTLCompareFunctionEqual;
    if ([func isEqualToString:@"D3DCMP_LESSEQUAL"]) return MTLCompareFunctionLessEqual;
    if ([func isEqualToString:@"D3DCMP_GREATER"]) return MTLCompareFunctionGreater;
    if ([func isEqualToString:@"D3DCMP_NOTEQUAL"]) return MTLCompareFunctionNotEqual;
    if ([func isEqualToString:@"D3DCMP_GREATEREQUAL"]) return MTLCompareFunctionGreaterEqual;
    return MTLCompareFunctionAlways;
}

static MTLCullMode cull_mode_from_d3d9(NSString *mode) {
    if ([mode isEqualToString:@"D3DCULL_CW"] || [mode isEqualToString:@"D3DCULL_CCW"]) return MTLCullModeBack;
    return MTLCullModeNone;
}

static MTLWinding winding_from_d3d9_cull(NSString *mode) {
    if ([mode isEqualToString:@"D3DCULL_CCW"]) return MTLWindingClockwise;
    return MTLWindingCounterClockwise;
}

static MTLPrimitiveType primitive_type_from_request(NSDictionary *req) {
    NSString *topology = req[@"topology"];
    NSDictionary *pipeline = [req[@"pipeline"] isKindOfClass:[NSDictionary class]] ? req[@"pipeline"] : @{};
    if (![topology isKindOfClass:[NSString class]]) topology = pipeline[@"primitive_topology"];
    if ([topology isEqualToString:@"trianglestrip"] || [topology isEqualToString:@"D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP"]) {
        return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}

static NSString *alpha_test_condition(NSString *func, NSString *alphaRef) {
    if ([func isEqualToString:@"D3DCMP_NEVER"]) return @"false";
    if ([func isEqualToString:@"D3DCMP_LESS"]) return [NSString stringWithFormat:@"color.a < %@", alphaRef];
    if ([func isEqualToString:@"D3DCMP_EQUAL"]) return [NSString stringWithFormat:@"color.a == %@", alphaRef];
    if ([func isEqualToString:@"D3DCMP_LESSEQUAL"]) return [NSString stringWithFormat:@"color.a <= %@", alphaRef];
    if ([func isEqualToString:@"D3DCMP_GREATER"]) return [NSString stringWithFormat:@"color.a > %@", alphaRef];
    if ([func isEqualToString:@"D3DCMP_NOTEQUAL"]) return [NSString stringWithFormat:@"color.a != %@", alphaRef];
    if ([func isEqualToString:@"D3DCMP_GREATEREQUAL"]) return [NSString stringWithFormat:@"color.a >= %@", alphaRef];
    return @"true";
}

static double rgba_channel(NSArray *rgba, NSUInteger index, double fallback) {
    if (![rgba isKindOfClass:[NSArray class]] || [rgba count] <= index) return fallback;
    return [[rgba objectAtIndex:index] doubleValue] / 255.0;
}

static NSString *float4_rgba_literal(NSArray *rgba) {
    return [NSString stringWithFormat:@"float4(%0.9f,%0.9f,%0.9f,%0.9f)",
        rgba_channel(rgba, 0, 1.0),
        rgba_channel(rgba, 1, 1.0),
        rgba_channel(rgba, 2, 1.0),
        rgba_channel(rgba, 3, 1.0)];
}

static NSString *d3d9_arg_expr(NSString *arg) {
    NSString *expr = @"diffuse";
    if ([arg containsString:@"D3DTA_TEXTURE"]) expr = @"texel";
    else if ([arg containsString:@"D3DTA_TFACTOR"]) expr = @"tfactor";
    else if ([arg containsString:@"D3DTA_CURRENT"]) expr = @"texel";
    if ([arg containsString:@"D3DTA_COMPLEMENT"]) {
        expr = [NSString stringWithFormat:@"(float4(1.0)-(%@))", expr];
    }
    if ([arg containsString:@"D3DTA_ALPHAREPLICATE"]) {
        expr = [NSString stringWithFormat:@"float4((%@).a)", expr];
    }
    return expr;
}

static NSString *d3d9_op_expr(NSString *op, NSString *lhs, NSString *rhs) {
    if ([op isEqualToString:@"D3DTOP_SELECTARG2"]) return rhs;
    if ([op isEqualToString:@"D3DTOP_MODULATE"]) return [NSString stringWithFormat:@"((%@)*(%@))", lhs, rhs];
    if ([op isEqualToString:@"D3DTOP_MODULATE2X"]) return [NSString stringWithFormat:@"saturate((%@)*(%@)*2.0)", lhs, rhs];
    if ([op isEqualToString:@"D3DTOP_ADD"]) return [NSString stringWithFormat:@"saturate((%@)+(%@))", lhs, rhs];
    if ([op isEqualToString:@"D3DTOP_ADDSIGNED"]) return [NSString stringWithFormat:@"saturate((%@)+(%@)-0.5)", lhs, rhs];
    if ([op isEqualToString:@"D3DTOP_ADDSIGNED2X"]) return [NSString stringWithFormat:@"saturate(((%@)+(%@)-0.5)*2.0)", lhs, rhs];
    if ([op isEqualToString:@"D3DTOP_ADDSMOOTH"]) return [NSString stringWithFormat:@"saturate((%@)+(%@)-((%@)*(%@)))", lhs, rhs, lhs, rhs];
    if ([op isEqualToString:@"D3DTOP_SUBTRACT"]) return [NSString stringWithFormat:@"saturate((%@)-(%@))", lhs, rhs];
    if ([op isEqualToString:@"D3DTOP_BLENDDIFFUSEALPHA"]) {
        return [NSString stringWithFormat:@"((%@)*diffuse.a+(%@)*(1.0-diffuse.a))", lhs, rhs];
    }
    if ([op isEqualToString:@"D3DTOP_BLENDTEXTUREALPHA"]) {
        return [NSString stringWithFormat:@"((%@)*texel.a+(%@)*(1.0-texel.a))", lhs, rhs];
    }
    return lhs;
}

static NSString *fragment_shader_source(BOOL textureMode, NSDictionary *req) {
    NSString *sourceApi = req[@"source_api"] ?: @"";
    NSString *shader = req[@"shader"];
    NSDictionary *d3d9 = [req[@"d3d9"] isKindOfClass:[NSDictionary class]] ? req[@"d3d9"] : @{};
    NSDictionary *ffp = [d3d9[@"ffp_shader"] isKindOfClass:[NSDictionary class]] ? d3d9[@"ffp_shader"] : @{};
    NSString *colorOp = ffp[@"color_op"] ?: @"";
    NSString *alphaOp = ffp[@"alpha_op"] ?: @"D3DTOP_SELECTARG1";
    NSString *colorArg1 = ffp[@"color_arg1"] ?: @"D3DTA_DIFFUSE";
    NSString *colorArg2 = ffp[@"color_arg2"] ?: @"D3DTA_TEXTURE";
    NSString *alphaArg1 = ffp[@"alpha_arg1"] ?: colorArg1;
    NSString *alphaArg2 = ffp[@"alpha_arg2"] ?: colorArg2;
    NSArray *textureFactor = [ffp[@"texture_factor"] isKindOfClass:[NSArray class]] ? ffp[@"texture_factor"] : @[@255, @255, @255, @255];
    NSString *tfactor = float4_rgba_literal(textureFactor);
    BOOL d3d9Mode = [sourceApi isEqualToString:@"d3d9"] || [sourceApi isEqualToString:@"d3d8"];
    BOOL programmableTextureModulate = [shader isEqualToString:@"d3d9-programmable-texture-modulate"];
    BOOL alphaTest = [ffp[@"alpha_test_enable"] boolValue];
    double alphaRef = [ffp[@"alpha_ref"] doubleValue] / 255.0;
    NSString *alphaFunc = ffp[@"alpha_func"] ?: @"D3DCMP_ALWAYS";
    NSString *colorExpr = (!d3d9Mode && textureMode)
        ? @"texel"
        : (!d3d9Mode ? @"diffuse" : (programmableTextureModulate
        ? @"(texel*diffuse)"
        : d3d9_op_expr(colorOp, d3d9_arg_expr(colorArg1), d3d9_arg_expr(colorArg2))));
    NSString *alphaExpr = (!d3d9Mode && textureMode)
        ? @"texel"
        : (!d3d9Mode ? @"diffuse" : (programmableTextureModulate
        ? @"(texel*diffuse)"
        : d3d9_op_expr(alphaOp, d3d9_arg_expr(alphaArg1), d3d9_arg_expr(alphaArg2))));
    NSString *alphaCode = @"";
    if (alphaTest) {
        NSString *condition = alpha_test_condition(alphaFunc, [NSString stringWithFormat:@"%0.9f", alphaRef]);
        alphaCode = [NSString stringWithFormat:@"if (!(%@)) discard_fragment();", condition];
    }
    if (textureMode) {
        return [NSString stringWithFormat:
            @"fragment float4 ps(O in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]){float4 diffuse=in.color;float4 texel=tex.sample(smp,in.uv);float4 tfactor=%@;float4 color_part=%@;float4 alpha_part=%@;float4 color=float4(color_part.rgb,alpha_part.a);%@ return color;}\n",
            tfactor, colorExpr, alphaExpr, alphaCode];
    }
    return [NSString stringWithFormat:
        @"fragment float4 ps(O in [[stage_in]]){float4 diffuse=in.color;float4 texel=diffuse;float4 tfactor=%@;float4 color_part=%@;float4 alpha_part=%@;float4 color=float4(color_part.rgb,alpha_part.a);%@ return color;}\n",
        tfactor, colorExpr, alphaExpr, alphaCode];
}

static NSString *fnv1a_hex(NSData *data) {
    const uint8_t *bytes = data.bytes;
    uint64_t hash = 1469598103934665603ULL;
    for (NSUInteger i = 0; i < data.length; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return [NSString stringWithFormat:@"%016llx", hash];
}

static BOOL write_ppm(NSString *path, NSUInteger width, NSUInteger height, NSData *rgba) {
    NSMutableData *ppm = [NSMutableData data];
    NSString *header = [NSString stringWithFormat:@"P6\n%lu %lu\n255\n", (unsigned long)width, (unsigned long)height];
    [ppm appendData:[header dataUsingEncoding:NSASCIIStringEncoding]];
    const uint8_t *src = rgba.bytes;
    for (NSUInteger i = 0; i < width * height; i++) {
        uint8_t rgb[3] = { src[i * 4], src[i * 4 + 1], src[i * 4 + 2] };
        [ppm appendBytes:rgb length:3];
    }
    return [ppm writeToFile:path atomically:YES];
}

static NSData *readback(id<MTLTexture> texture, NSUInteger width, NSUInteger height) {
    NSUInteger bytesPerRow = width * 4;
    NSMutableData *data = [NSMutableData dataWithLength:bytesPerRow * height];
    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [texture getBytes:data.mutableBytes bytesPerRow:bytesPerRow fromRegion:region mipmapLevel:0];
    return data;
}

static int render_request(NSString *requestPath) {
    NSData *jsonData = [NSData dataWithContentsOfFile:requestPath];
    if (!jsonData) { fprintf(stderr, "request read failed\n"); return 2; }
    NSError *error = nil;
    NSDictionary *req = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
    if (!req || error) { fprintf(stderr, "request json failed\n"); return 3; }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "Metal device unavailable\n"); return 4; }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) { fprintf(stderr, "Metal command queue unavailable\n"); return 5; }

    NSUInteger width = [req[@"width"] unsignedIntegerValue] ?: 64;
    NSUInteger height = [req[@"height"] unsignedIntegerValue] ?: 64;
    NSString *mode = req[@"mode"] ?: @"clear";
    NSDictionary *d3d9 = [req[@"d3d9"] isKindOfClass:[NSDictionary class]] ? req[@"d3d9"] : @{};
    NSDictionary *depthInfo = [d3d9[@"depth_state"] isKindOfClass:[NSDictionary class]] ? d3d9[@"depth_state"] : @{};
    BOOL depthEnabled = [depthInfo[@"z_enable"] boolValue];
    NSString *output = req[@"output"];
    NSString *report = req[@"report"];
    NSArray *cc = req[@"clear_color"] ?: @[ @0, @0, @0, @255 ];
    double r = [cc[0] doubleValue];
    double g = [cc[1] doubleValue];
    double b = [cc[2] doubleValue];
    double a = [cc[3] doubleValue];
    MTLClearColor clear = MTLClearColorMake(clamp_channel(r) / 255.0, clamp_channel(g) / 255.0, clamp_channel(b) / 255.0, clamp_channel(a) / 255.0);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    if (!texture) { fprintf(stderr, "Metal texture unavailable\n"); return 6; }
    id<MTLTexture> depthTexture = nil;
    if (depthEnabled) {
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:width height:height mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModePrivate;
        depthTexture = [device newTextureWithDescriptor:dd];
        if (!depthTexture) { fprintf(stderr, "Metal depth texture unavailable\n"); return 6; }
    }

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = clear;
    if (depthTexture) {
        rp.depthAttachment.texture = depthTexture;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
        rp.depthAttachment.clearDepth = 1.0;
    }
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

    NSArray *vp = req[@"viewport"];
    if ([vp isKindOfClass:[NSArray class]] && [vp count] == 4) {
        MTLViewport viewport = {
            [vp[0] doubleValue], [vp[1] doubleValue],
            [vp[2] doubleValue], [vp[3] doubleValue],
            0.0, 1.0
        };
        [enc setViewport:viewport];
    }
    NSArray *sc = req[@"scissor"];
    if ([sc isKindOfClass:[NSArray class]] && [sc count] == 4) {
        MTLScissorRect rect = {
            (NSUInteger)[sc[0] unsignedIntegerValue],
            (NSUInteger)[sc[1] unsignedIntegerValue],
            (NSUInteger)[sc[2] unsignedIntegerValue],
            (NSUInteger)[sc[3] unsignedIntegerValue]
        };
        [enc setScissorRect:rect];
    }

    if ([mode isEqualToString:@"triangle"] || [mode isEqualToString:@"indexed_triangle"] || [mode isEqualToString:@"texture"]) {
        BOOL textureMode = [mode isEqualToString:@"texture"];
        NSString *src = [@"#include <metal_stdlib>\nusing namespace metal;\nstruct V{packed_float4 p; packed_float4 c; packed_float2 uv;}; struct O{float4 position [[position]]; float4 color; float2 uv;};\nvertex O vs(uint id [[vertex_id]], const device V* v [[buffer(0)]]){O o; o.position=float4(v[id].p); o.color=float4(v[id].c); o.uv=float2(v[id].uv); return o;}\n" stringByAppendingString:fragment_shader_source(textureMode, req)];
        id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&error];
        if (!lib) { fprintf(stderr, "Metal library failed: %s\n", error.localizedDescription.UTF8String); return 7; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"ps"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        NSDictionary *renderStates = [d3d9[@"render_states"] isKindOfClass:[NSDictionary class]] ? d3d9[@"render_states"] : @{};
        pd.colorAttachments[0].writeMask = color_write_mask_from_d3d9(renderStates[@"D3DRS_COLORWRITEENABLE"]);
        if (depthEnabled) {
            pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        }
        NSDictionary *ffp = [d3d9[@"ffp_shader"] isKindOfClass:[NSDictionary class]] ? d3d9[@"ffp_shader"] : @{};
        if ([ffp[@"alpha_blend_enable"] boolValue]) {
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = blend_factor_from_d3d9(ffp[@"src_blend"] ?: @"D3DBLEND_ONE");
            pd.colorAttachments[0].destinationRGBBlendFactor = blend_factor_from_d3d9(ffp[@"dest_blend"] ?: @"D3DBLEND_ZERO");
            pd.colorAttachments[0].sourceAlphaBlendFactor = pd.colorAttachments[0].sourceRGBBlendFactor;
            pd.colorAttachments[0].destinationAlphaBlendFactor = pd.colorAttachments[0].destinationRGBBlendFactor;
            pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
            pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        }
        id<MTLRenderPipelineState> ps = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!ps) { fprintf(stderr, "Metal pipeline failed: %s\n", error.localizedDescription.UTF8String); return 8; }
        id<MTLDepthStencilState> depthState = nil;
        if (depthEnabled) {
            MTLDepthStencilDescriptor *depthDesc = [MTLDepthStencilDescriptor new];
            depthDesc.depthCompareFunction = compare_function_from_d3d9(depthInfo[@"z_func"] ?: @"D3DCMP_LESSEQUAL");
            depthDesc.depthWriteEnabled = [depthInfo[@"z_write_enable"] boolValue];
            depthState = [device newDepthStencilStateWithDescriptor:depthDesc];
        }
        NSMutableData *vertexData = vertex_data_from_request(req[@"vertices"], d3d9[@"wvp_matrix"]);
        BOOL index32 = [req[@"index_format"] isEqualToString:@"uint32"];
        NSMutableData *indexData = index_data_from_request(req[@"indices"], index32);
        NSUInteger vertexCount = [vertexData length] / sizeof(struct V);
        NSUInteger indexCount = [indexData length] / (index32 ? sizeof(uint32_t) : sizeof(uint16_t));
        id<MTLBuffer> vb = [device newBufferWithBytes:[vertexData bytes] length:[vertexData length] options:MTLResourceStorageModeShared];
        [enc setRenderPipelineState:ps];
        if (depthState) [enc setDepthStencilState:depthState];
        NSString *cullMode = renderStates[@"D3DRS_CULLMODE"] ?: @"D3DCULL_NONE";
        [enc setFrontFacingWinding:winding_from_d3d9_cull(cullMode)];
        [enc setCullMode:cull_mode_from_d3d9(cullMode)];
        [enc setVertexBuffer:vb offset:0 atIndex:0];
        if (textureMode) {
            NSArray *textureSize = req[@"texture_size"];
            NSUInteger sampleWidth = [textureSize isKindOfClass:[NSArray class]] && [textureSize count] > 0 ? [[textureSize objectAtIndex:0] unsignedIntegerValue] : 2;
            NSUInteger sampleHeight = [textureSize isKindOfClass:[NSArray class]] && [textureSize count] > 1 ? [[textureSize objectAtIndex:1] unsignedIntegerValue] : 2;
            NSMutableData *texels = texture_data_from_request(req[@"texture_pixels"]);
            MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:sampleWidth height:sampleHeight mipmapped:NO];
            sd.usage = MTLTextureUsageShaderRead;
            sd.storageMode = MTLStorageModeShared;
            id<MTLTexture> sampleTexture = [device newTextureWithDescriptor:sd];
            [sampleTexture replaceRegion:MTLRegionMake2D(0, 0, sampleWidth, sampleHeight) mipmapLevel:0 withBytes:[texels bytes] bytesPerRow:sampleWidth * 4];
            MTLSamplerDescriptor *sdesc = [MTLSamplerDescriptor new];
            NSDictionary *samplerInfo = [d3d9[@"sampler"] isKindOfClass:[NSDictionary class]] ? d3d9[@"sampler"] : @{};
            sdesc.sAddressMode = address_mode_from_d3d9(samplerInfo[@"address_u"] ?: @"D3DTADDRESS_CLAMP");
            sdesc.tAddressMode = address_mode_from_d3d9(samplerInfo[@"address_v"] ?: @"D3DTADDRESS_CLAMP");
            sdesc.minFilter = filter_from_d3d9(samplerInfo[@"min_filter"] ?: @"D3DTEXF_POINT");
            sdesc.magFilter = filter_from_d3d9(samplerInfo[@"mag_filter"] ?: @"D3DTEXF_POINT");
            id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sdesc];
            [enc setFragmentTexture:sampleTexture atIndex:0];
            [enc setFragmentSamplerState:sampler atIndex:0];
        }
        if (indexCount > 0) {
            id<MTLBuffer> ib = [device newBufferWithBytes:[indexData bytes] length:[indexData length] options:MTLResourceStorageModeShared];
            [enc drawIndexedPrimitives:primitive_type_from_request(req) indexCount:indexCount indexType:(index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16) indexBuffer:ib indexBufferOffset:0];
        } else {
            [enc drawPrimitives:primitive_type_from_request(req) vertexStart:0 vertexCount:vertexCount];
        }
    }
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "Metal command failed: %s\n", cb.error.localizedDescription.UTF8String); return 9; }

    NSData *rgba = readback(texture, width, height);
    if (![output length] || !write_ppm(output, width, height, rgba)) { fprintf(stderr, "PPM write failed\n"); return 10; }
    const uint8_t *px = rgba.bytes;
    uint8_t bg[4] = { clamp_channel(r), clamp_channel(g), clamp_channel(b), clamp_channel(a) };
    NSUInteger nonBg = 0;
    for (NSUInteger i = 0; i < width * height; i++) {
        if (px[i*4] != bg[0] || px[i*4+1] != bg[1] || px[i*4+2] != bg[2] || px[i*4+3] != bg[3]) nonBg++;
    }
    NSDictionary *payload = @{ @"backend": @"metal", @"status": @"PASS", @"width": @(width), @"height": @(height), @"checksum": fnv1a_hex(rgba), @"non_background_pixels": @(nonBg), @"ppm_path": output };
    NSData *out = [NSJSONSerialization dataWithJSONObject:payload options:NSJSONWritingPrettyPrinted error:nil];
    [out writeToFile:report atomically:YES];
    return 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--probe") == 0) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) { printf("device=no\n"); return 1; }
            printf("device=yes name=%s\n", device.name.UTF8String);
            return 0;
        }
        if (argc == 3 && strcmp(argv[1], "--request") == 0) {
            return render_request([NSString stringWithUTF8String:argv[2]]);
        }
        fprintf(stderr, "usage: metal_render --probe | --request file.json\n");
        return 64;
    }
}
