// This file prepares server icons and emojis in a browser worker before uploading them.

var image_upload =
{
    worker: null,
    queue: Promise.resolve(),
    idle_timer: null
};

/**
 * @brief Reads and resizes images in a separate thread without loading the rest of the chat.
 */
function image_upload__worker_main()
{
    /**
     * @brief Converts the finished image to the format used by upload messages.
     * @param blob -> resized image or unchanged original file
     */
    async function to_data_url(blob)
    {
        let bytes = new Uint8Array(await blob.arrayBuffer());
        let content = "";
        for (let offset = 0; offset < bytes.length; offset += 8192)
        {
            content += String.fromCharCode(...bytes.subarray(offset, offset + 8192));
        }
        return "data:" + blob.type + ";base64," + btoa(content);
    }

    self.onmessage = async function(event)
    {
        let file = event.data.file;
        let kind = event.data.kind;
        let maximum_bytes = event.data.maximum_bytes;
        let maximum_dimension = kind === "icon" ? 64 : 128;
        let bitmap = null;
        try
        {
            if (file.type === "image/webp")
            {
                let header = new Uint8Array(await file.slice(0, 30).arrayBuffer());
                if (header.length < 20 || String.fromCharCode(...header.slice(0, 4)) !== "RIFF"
                    || String.fromCharCode(...header.slice(8, 12)) !== "WEBP")
                {
                    throw new Error("Choose a valid WebP image.");
                }
                if (String.fromCharCode(...header.slice(12, 16)) === "VP8X" && (header[20] & 2))
                {
                    throw new Error("Animated WebP is not supported here. Use a GIF for animated emojis.");
                }
            }
            if (file.type === "image/gif")
            {
                let bytes = new Uint8Array(await file.slice(0, 14).arrayBuffer());
                let signature = String.fromCharCode(...bytes.slice(0, 6));
                if (bytes.length < 14 || (signature !== "GIF87a" && signature !== "GIF89a"))
                {
                    throw new Error("Choose a valid GIF image.");
                }
                let width = bytes[6] | (bytes[7] << 8);
                let height = bytes[8] | (bytes[9] << 8);
                if (!width || !height || width > 128 || height > 128)
                {
                    throw new Error("GIF emojis must be at most 128 × 128 pixels.");
                }
            }
            if (typeof createImageBitmap !== "function")
            {
                throw new Error("This browser cannot prepare images in a separate thread.");
            }
            bitmap = await createImageBitmap(file);
            let ratio = Math.min(1, maximum_dimension / bitmap.width, maximum_dimension / bitmap.height);
            let output = file;
            if (file.type === "image/png" || file.type === "image/webp"
                || (file.type === "image/jpeg" && (ratio < 1 || file.size > maximum_bytes)))
            {
                if (typeof OffscreenCanvas !== "function")
                {
                    throw new Error("This browser cannot resize images in a separate thread.");
                }
                let canvas = new OffscreenCanvas(1, 1);
                let context = canvas.getContext("2d");
                if (!context)
                {
                    throw new Error("Could not prepare this image.");
                }
                for (let attempt = 0; attempt < 24; attempt++)
                {
                    canvas.width = Math.max(1, Math.floor(bitmap.width * ratio));
                    canvas.height = Math.max(1, Math.floor(bitmap.height * ratio));
                    context.imageSmoothingEnabled = true;
                    context.imageSmoothingQuality = "high";
                    context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
                    output = await canvas.convertToBlob({ type: file.type === "image/jpeg" ? "image/jpeg" : "image/png", quality: 0.86 });
                    if (output.size <= maximum_bytes)
                    {
                        break;
                    }
                    ratio *= 0.8;
                }
                if (output.size > maximum_bytes)
                {
                    throw new Error("This image could not be reduced to the server's size limit.");
                }
            }
            self.postMessage({ data: await to_data_url(output) });
        }
        catch (error)
        {
            self.postMessage({ error: error.message || "Could not read this image." });
        }
        finally
        {
            if (bitmap)
            {
                bitmap.close();
            }
        }
    };
}

/**
 * @brief Frees the image worker after inactivity or a processing failure.
 */
function image_upload__stop_worker()
{
    clearTimeout(image_upload.idle_timer);
    image_upload.idle_timer = null;
    if (image_upload.worker)
    {
        image_upload.worker.terminate();
        image_upload.worker = null;
    }
}

/**
 * @brief Sends one image to the worker and waits for its result.
 * @param file -> selected image
 * @param kind -> icon or emoji
 * @param maximum_bytes -> largest allowed static image after resizing
 */
function image_upload__run(file, kind, maximum_bytes)
{
    return new Promise(function(resolve, reject)
    {
        let timer = null;
        let worker = null;
        clearTimeout(image_upload.idle_timer);

        function fail(message)
        {
            clearTimeout(timer);
            image_upload__stop_worker();
            reject(new Error(message));
        }

        try
        {
            if (!image_upload.worker)
            {
                let source = "(" + image_upload__worker_main.toString() + ")();";
                let url = URL.createObjectURL(new Blob([source], { type: "text/javascript" }));
                try
                {
                    image_upload.worker = new Worker(url);
                }
                finally
                {
                    URL.revokeObjectURL(url);
                }
            }
            worker = image_upload.worker;
            worker.onmessage = function(event)
            {
                clearTimeout(timer);
                worker.onmessage = null;
                worker.onerror = null;
                worker.onmessageerror = null;
                image_upload.idle_timer = setTimeout(image_upload__stop_worker, 30000);
                if (event.data.error)
                {
                    reject(new Error(event.data.error));
                }
                else
                {
                    resolve(event.data.data);
                }
            };
            worker.onerror = function(event)
            {
                event.preventDefault();
                fail("Could not start or run the image processing thread.");
            };
            worker.onmessageerror = function()
            {
                fail("Could not read the image processing result.");
            };
            timer = setTimeout(function()
            {
                fail("Preparing this image took too long. Try a smaller image.");
            }, 30000);
            worker.postMessage({ file: file, kind: kind, maximum_bytes: maximum_bytes });
        }
        catch (error)
        {
            fail(error.message || "Could not start the image processing thread.");
        }
    });
}

/**
 * @brief Prepares icons and emojis one at a time without interrupting the chat or audio threads.
 * @param file -> PNG, JPEG or WebP image, or a GIF for emojis
 * @param kind -> icon or emoji
 * @param maximum_bytes -> server size limit for the finished static image
 * @return Promise -> image data URL ready for upload
 */
function image_upload__prepare(file, kind, maximum_bytes = 30000)
{
    let types = kind === "emoji" ? ["image/png", "image/jpeg", "image/webp", "image/gif"] : ["image/png", "image/jpeg", "image/webp"];
    if (!file || !types.includes(file.type) || (kind !== "icon" && kind !== "emoji"))
    {
        return Promise.reject(new Error(kind === "emoji" ? "Choose a PNG, JPEG, WebP or GIF image." : "Choose a PNG, JPEG or WebP image."));
    }
    if (file.type === "image/gif" && file.size > 256 * 1024)
    {
        return Promise.reject(new Error("GIF emojis must be at most 256 KiB."));
    }
    if (file.size > 10 * 1024 * 1024)
    {
        return Promise.reject(new Error("PNG, JPEG and WebP files must be at most 10 MiB."));
    }
    if (!Number.isInteger(maximum_bytes) || maximum_bytes < 1000 || maximum_bytes > 30000)
    {
        return Promise.reject(new Error("The server sent an invalid image size limit."));
    }
    let result = image_upload.queue.then(function()
    {
        return image_upload__run(file, kind, maximum_bytes);
    });
    image_upload.queue = result.catch(function()
    {
        // A rejected image does not prevent the next selected image from being processed.
    });
    return result;
}
