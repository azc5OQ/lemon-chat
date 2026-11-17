package com.lemonchat.ui;

import com.lemonchat.model.PermissionDefinition;
import java.util.LinkedHashMap;
import java.util.Map;

/** keeps permission edits separate from the service snapshot */
final class PermissionDraft
{
    final PermissionDefinition original;
    private final Map<String, String> actions;
    private final Map<String, Integer> powers;
    private final Map<String, Integer> grants;
    private final Map<String, PermissionDefinition.Flags> flags;

    PermissionDraft(PermissionDefinition original)
    {
        this.original = original;
        actions = new LinkedHashMap<>(original.actions);
        powers = new LinkedHashMap<>(original.powers);
        grants = new LinkedHashMap<>(original.grants);
        flags = new LinkedHashMap<>(original.flags);
    }

    PermissionDefinition value()
    {
        return new PermissionDefinition(actions, powers, grants, flags);
    }

    boolean changed()
    {
        return !original.sameValues(value());
    }

    /**
     * changes one known setting while retaining the other values
     *
     * @param field setting selected from the source catalog
     * @param value action decision or numeric power
     * @param required editing power needed to change this setting
     * @param options advanced combination rules
     */
    void update(PermissionFields.Field field, String value, int required, PermissionDefinition.Flags options)
    {
        if ((field.power ? !powers.containsKey(field.name) : !actions.containsKey(field.name)) || required < 0 || required > 1000 || options == null || options.skip && !field.skip)
        {
            throw new IllegalArgumentException("This setting cannot use those values");
        }
        if (field.power)
        {
            int number = number(value);
            powers.put(field.name, number);
        }
        else
        {
            if (!value.equals("grant") && !value.equals("deny") && (!value.equals("default") || field.channel))
            {
                throw new IllegalArgumentException(field.channel ? "Choose Enable or Disable" : "Choose Enable, Disable or Default");
            }
            actions.put(field.name, value);
        }
        grants.put(field.name, required);
        if (options.skip || options.negate)
        {
            flags.put(field.name, options);
        }
        else
        {
            flags.remove(field.name);
        }
    }

    static int number(String value)
    {
        if (value == null || !value.matches("[0-9]{1,4}"))
        {
            throw new IllegalArgumentException("Enter a whole number from 0 to 1000");
        }
        int result = Integer.parseInt(value);
        if (result > 1000)
        {
            throw new IllegalArgumentException("Enter a whole number from 0 to 1000");
        }
        return result;
    }
}
